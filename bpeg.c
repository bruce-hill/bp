/*
 * bpeg.c - Source code for the bpeg parser
 *
 * Grammar:
 *     # <comment>                 comment
 *     .                           any character (multiline: $.)
 *     ^                           beginning of a line (^^: beginning of file)
 *     $                           end of a line ($$: end of file)
 *     _                           0 or more spaces or tabs (__: include newlines and comments)
 *     `<c>                        character <c>
 *     `<a>-<z>                    character between <a> and <z>
 *     \<e>                        escape sequence (e.g. \n, \033)
 *     \<e1>-<e2>                  escape sequence range (e.g. \x00-\xF0)
 *     ! <pat>                     no <pat>
 *     ~ <pat>                     any character as long as it doesn't match <pat> (multiline: ~~<pat>)
 *     & <pat>                     upto and including <pat> (aka *~<pat> <pat>) (multiline: &&<pat>)
 *     <N=1> + <pat> [% <sep="">]  <N> or more <pat>s (separated by <sep>)
 *     * <pat> [% <sep="">]        sugar for "0+ <pat> [% <sep>]"
 *     <N=1> - <pat> [% <sep="">]  <N> or fewer <pat>s (separated by <sep>)
 *     ? <pat>                     sugar for "1- <pat>"
 *     <N> - <M> <pat>             <N> to <M> (inclusive) <pat>s
 *     < <pat>                     after <pat>, ...
 *     > <pat>                     before <pat>, ...
 *     ( <pat> )                   <pat>
 *     @ <pat>                     capture <pat>
 *     @ [ <name> ] <pat>          <pat> named <name>
 *     { <pat> => <str> }           <pat> replaced with <str>
 *     "@1" or "@[1]"              first capture
 *     "@foo" or "@[foo]"          capture named "foo"
 *     <pat1> <pat2>               <pat1> followed by <pat2>
 *     <pat> / <alt>               <pat> otherwise <alt>
 *     ; <name> = <pat>            <name> is defined to be <pat>
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "compiler.h"
#include "grammar.h"
#include "utils.h"
#include "vm.h"

static const char *usage = (
    "Usage:\n"
    "  bpeg [flags] <pattern> [<input files>...]\n\n"
    "Flags:\n"
    "  -h --help\t print the usage and quit\n"
    "  -v --verbose\t print verbose debugging info\n"
    "  -s --slow\t run in slow mode for debugging\n"
    "  -r --replace <replacement>   replace the input pattern with the given replacement\n"
    "  -g --grammar <grammar file>  use the specified file as a grammar\n");


int main(int argc, char *argv[])
{
    int verbose = 0;
    const char *pattern = NULL,
          *replacement = NULL,
          *grammarfile = NULL,
          *infile = NULL;

    grammar_t *g = new_grammar();

    for (int i = 1; i < argc; i++) {
        if (streq(argv[i], "--help") || streq(argv[i], "-h")) {
            printf("%s\n", usage);
            return 0;
        } else if (streq(argv[i], "--verbose") || streq(argv[i], "-v")) {
            verbose = 1;
        } else if (streq(argv[i], "--replace") || streq(argv[i], "-r")) {
            replacement = argv[++i];
        } else if (streq(argv[i], "--grammar") || streq(argv[i], "-g")) {
            grammarfile = argv[++i];
        } else if (streq(argv[i], "--define") || streq(argv[i], "-d")) {
            char *def = argv[++i];
            char *eq = strchr(def, '=');
            check(eq, usage);
            *eq = '\0';
            char *src = ++eq;
            vm_op_t *pat = bpeg_pattern(src);
            check(pat, "Failed to compile pattern");
            add_def(g, src, def, pat);
        } else if (pattern == NULL) {
            pattern = argv[i];
        } else if (infile == NULL) {
            infile = argv[i];
        }
    }

    check(pattern != NULL || grammarfile != NULL, usage);

    if (grammarfile) {
        // load grammar from a file (semicolon mode)
        char *grammar;
        if (streq(grammarfile, "-")) {
            grammar = readfile(STDIN_FILENO);
        } else {
            int fd = open(grammarfile, O_RDONLY);
            check(fd >= 0, "Couldn't open file: %s", argv[2]);
            grammar = readfile(fd);
        }
        load_grammar(g, grammar);
    } else {
        // load grammar in start-with-string mode:
        vm_op_t *pat = bpeg_stringpattern(pattern);
        if (replacement)
            pat = bpeg_replacement(pat, replacement);

        add_def(g, pattern, "pattern", pat);

        const char *grammar = "find = *(@pattern / \\n / .);";
        load_grammar(g, grammar);
    }

    if (verbose) {
        print_pattern(g->pattern);
    }

    char *input;
    if (infile == NULL || streq(infile, "-")) {
        input = readfile(STDIN_FILENO);
    } else {
        int fd = open(infile, O_RDONLY);
        check(fd >= 0, "Couldn't open file: %s", argv[2]);
        input = readfile(fd);
    }

    // Ensure string has a null byte to the left:
    char *lpadded = calloc(sizeof(char), strlen(input)+2);
    stpcpy(&lpadded[1], input);
    input = &lpadded[1];

    match_t *m = match(g, input, g->pattern);
    if (m == NULL) {
        printf("No match\n");
        return 1;
    } else {
        print_match(m, "\033[0m", verbose);
        printf("\033[0;2m%s\n", m->end);
    }

    return 0;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
