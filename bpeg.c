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
#include <limits.h>
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
    "  -d --define <name>=<def>     Define a grammar rule\n"
    "  -r --replace <replacement>   replace the input pattern with the given replacement\n"
    "  -g --grammar <grammar file>  use the specified file as a grammar\n");

static char *getflag(const char *flag, char *argv[], int *i)
{
    size_t n = strlen(flag);
    if (strncmp(argv[*i], flag, n) == 0) {
        if (argv[*i][n] == '=') {
            return &argv[*i][n+1];
        } else if (argv[*i][n] == '\0') {
            ++(*i);
            return argv[*i];
        }
    }
    return NULL;
}
#define FLAG(f) (flag=getflag((f), argv, &i))

int main(int argc, char *argv[])
{
    int verbose = 0;
    char *flag = NULL;
    const char *rule = "find-all";

    grammar_t *g = new_grammar();

    int fd;
    if ((fd=open("/etc/xdg/bpeg/builtins.bpeg", O_RDONLY)) >= 0)
        load_grammar(g, readfile(fd)); // Keep in memory for debugging output
    char path[PATH_MAX] = {0};
    sprintf(path, "%s/.config/bpeg/builtins.bpeg", getenv("HOME"));
    if ((fd=open(path, O_RDONLY)) >= 0)
        load_grammar(g, readfile(fd)); // Keep in memory for debugging output

    int i, npatterns = 0;
    for (i = 1; i < argc; i++) {
        if (streq(argv[i], "--")) {
            ++i;
            break;
        } else if (FLAG("--help") || FLAG("-h")) {
            printf("%s\n", usage);
            return 0;
        } else if (FLAG("--verbose") || FLAG("-v")) {
            verbose = 1;
        } else if (FLAG("--replace") || FLAG("-r")) {
            vm_op_t *p = bpeg_replacement(bpeg_pattern("pattern"), flag);
            check(p, "Replacement failed to compile");
            add_def(g, flag, "replacement", p);
            rule = "replace-all";
        } else if (FLAG("--grammar") || FLAG("-g")) {
            int fd;
            if (streq(flag, "-")) {
                fd = STDIN_FILENO;
            } else {
                fd = open(flag, O_RDONLY);
                if (fd < 0) {
                    sprintf(path, "%s/.config/bpeg/%s.bpeg", getenv("HOME"), flag);
                    fd = open(path, O_RDONLY);
                }
                if (fd < 0) {
                    sprintf(path, "/etc/xdg/bpeg/%s.bpeg", flag);
                    fd = open(path, O_RDONLY);
                }
                check(fd >= 0, "Couldn't find grammar: %s", flag);
            }
            load_grammar(g, readfile(fd)); // Keep in memory for debug output
        } else if (FLAG("--define") || FLAG("-d")) {
            char *def = flag;
            char *eq = strchr(def, '=');
            check(eq, usage);
            *eq = '\0';
            char *src = ++eq;
            vm_op_t *pat = bpeg_pattern(src);
            check(pat, "Failed to compile pattern");
            add_def(g, src, def, pat);
        } else if (FLAG("--escaped") || FLAG("-e")) {
            check(npatterns == 0, "Cannot define multiple patterns");
            vm_op_t *p = bpeg_pattern(flag);
            check(p, "Pattern failed to compile: '%s'", flag);
            add_def(g, flag, "pattern", p);
            ++npatterns;
        } else if (FLAG("--string") || FLAG("-s")) {
            vm_op_t *p = bpeg_stringpattern(flag);
            check(p, "Pattern failed to compile");
            add_def(g, flag, "pattern", p);
            ++npatterns;
        } else if (FLAG("--mode") || FLAG("-m")) {
            rule = flag;
        } else if (argv[i][0] != '-') {
            if (npatterns > 0) break;
            vm_op_t *p = bpeg_stringpattern(argv[i]);
            check(p, "Pattern failed to compile");
            add_def(g, argv[i], "pattern", p);
            ++npatterns;
        } else {
            printf("Unrecognized flag: %s\n%s\n", argv[i], usage);
            return 1;
        }
    }

    vm_op_t *pattern = lookup(g, rule);
    check(pattern != NULL, "No such rule: '%s'", rule);

    if (verbose) {
        print_pattern(pattern);
    }

    char *input;
    if (i == argc) {
        // Force stdin if no files given
        input = readfile(STDIN_FILENO);
        goto run_match;
    }
    for (int nfiles = 0; i < argc; nfiles++, i++) {
        if (argv[i] == NULL || streq(argv[i], "-")) {
            input = readfile(STDIN_FILENO);
        } else {
            int fd = open(argv[i], O_RDONLY);
            check(fd >= 0, "Couldn't open file: %s", argv[i]);
            input = readfile(fd);
            if (nfiles > 0) printf("\n");
            printf("\033[1;4;33m%s\033[0m\n", argv[i]);
        }

      run_match:;
        // Ensure string has a null byte to the left:
        char *lpadded = calloc(sizeof(char), strlen(input)+2);
        stpcpy(&lpadded[1], input);
        input = &lpadded[1];

        match_t *m = match(g, input, pattern);
        if (m == NULL) {
            printf("No match\n");
            return 1;
        } else {
            print_match(m, isatty(STDOUT_FILENO) ? "\033[0m" : NULL, verbose);
            //printf("\033[0;2m%s\n", m->end);
        }
        freefile(input);
    }

    return 0;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
