/*
 * grammar.c - Code for defining grammars (sets of rules)
 */

#include "grammar.h"
#include "compiler.h"
#include "utils.h"

const char *BPEG_BUILTIN_GRAMMAR = (
    "crlf=\\r\\n;\n"
    "cr=\\r;\n" "r=\\r;\n"
    "anglebraces=`< *(anglebraces / ~~`>) `>;\n"
    "brackets=`[ *(brackets / ~~`]) `];\n"
    "braces=`{ *(braces / ~~`}) `};\n"
    "parens=`( *(parens / ~~`)) `);\n"
    "id=(`a-z/`A-Z/`_) *(`a-z/`A-Z/`_/`0-9);\n"
    "HEX=`0-9/`A-F;\n"
    "Hex=`0-9/`a-f/`A-F;\n"
    "hex=`0-9/`a-f;\n"
    "number=+`0-9 ?(`. *`0-9) / `. +`0-9;\n"
    "int=+`0-9;\n"
    "digit=`0-9;\n"
    "Abc=`a-z/`A-Z;\n"
    "ABC=`A-Z;\n"
    "abc=`a-z;\n"
    "esc=\\e;\n" "e=\\e;\n"
    "tab=\\t;\n" "t=\\t;\n"
    "nl=\\n;\n" "lf=\\n;\n" "n=\\n;\n"
    "c-block-comment='/*' &&'*/';\n"
    "c-line-comment='//' &$;\n"
    "c-comment=c-line-comment / c-block-comment;\n"
    "hash-comment=`# &$;\n"
    "comment=!(/);\n" // No default definition, can be overridden
    "WS=` /\\t/\\n/\\r/comment;\n"
    "ws=` /\\t;\n"
    "$$=!$.;\n"
    "$=!.;\n"
    "^^=!<$.;\n"
    "^=!<.;\n"
    "__=*(` /\\t/\\n/\\r/comment);\n"
    "_=*(` /\\t);\n"
    );

grammar_t *new_grammar(void)
{
    grammar_t *g = calloc(sizeof(grammar_t), 1);
    g->definitions = calloc(sizeof(def_t), (g->capacity = 128));
    load_grammar(g, BPEG_BUILTIN_GRAMMAR);
    return g;
}

void add_def(grammar_t *g, const char *src, const char *name, vm_op_t *op)
{
    if (g->size >= g->capacity) {
        g->definitions = realloc(g->definitions, (g->capacity += 32));
    }
    int i = g->size;
    g->definitions[i].source = src;
    g->definitions[i].name = name;
    g->definitions[i].op = op;
    ++g->size;
}

void load_grammar(grammar_t *g, const char *src)
{
    vm_op_t *mainpat = NULL;
    do {
        src = after_spaces(src);
        if (!*src) break;
        const char *name = src;
        const char *name_end = after_name(name);
        check(name_end > name, "Invalid name for definition");
        name = strndup(name, (size_t)(name_end-name));
        src = after_spaces(name_end);
        check(matchchar(&src, '='), "Expected '=' in definition");
        vm_op_t *op = bpeg_pattern(src);
        check(op, "Couldn't load definition");
        add_def(g, src, name, op);
        if (mainpat == NULL) {
            mainpat = op;
            g->pattern = op;
        }
        src = op->end;
    } while (*src && matchchar(&src, ';')); 
}

/*
 * Print a BPEG grammar in human-readable form.
 */
void print_grammar(grammar_t *g)
{
    if (g->pattern) print_pattern(g->pattern);
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
