/*
 * grammar.c - Code for defining grammars (sets of rules)
 */

#include "grammar.h"
#include "compiler.h"
#include "utils.h"

const char *BPEG_BUILTIN_GRAMMAR = (
    // Meta-rules for acting on everything
    "pattern = !(/);\n" // Not defined by default
    "replacement = {!(/)=>};\n" // Not defined by default
    "replace-all = *&&@replacement &&$$;\n"
    "find-all = *(matching-line / {&&(\\n/$$)=>});\n"
    "matching-line = +&@pattern *. $ ?\\n;\n"
    "only-matches = *{&&@pattern=>'@1\\n'};\n"

    // Helper definitions (commonly used)
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

/*
 * Load the given grammar (semicolon-separated definitions)
 * and return the first rule defined.
 */
vm_op_t *load_grammar(grammar_t *g, const char *src)
{
    vm_op_t *ret = NULL;
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
        if (ret == NULL) {
            ret = op;
        }
        src = op->end;
    } while (*src && matchchar(&src, ';')); 
    return ret;
}

vm_op_t *lookup(grammar_t *g, const char *name)
{
    // Search backwards so newer defs take precedence
    for (int i = g->size-1; i >= 0; i--) {
        if (streq(g->definitions[i].name, name)) {
            return g->definitions[i].op;
        }
    }
    return NULL;
}
