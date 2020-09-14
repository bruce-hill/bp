/*
 * bpeg.c - Source code for the bpeg parser
 *
 * See `man ./bpeg.1` for more details
 */
#include <fcntl.h>
#include <glob.h>
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
    "  -d --define <name>=<def>\t define a grammar rule\n"
    "  -D --define-string <name>=<def>\t define a grammar rule (string-pattern)\n"
    "  -p --pattern <pat>\t provide a pattern (equivalent to bpeg '\\(<pat>)')\n"
    "  -P --pattern-string <pat>\t provide a string pattern (equivalent to bpeg '<pat>', but may be useful if '<pat>' begins with a '-')\n"
    "  -r --replace <replacement>   replace the input pattern with the given replacement\n"
    "  -m --mode <mode>\t set the behavior mode (defult: find-all)\n"
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

static int run_match(grammar_t *g, const char *filename, vm_op_t *pattern, int verbose)
{
    char *input;
    if (filename == NULL || streq(filename, "-")) {
        input = readfile(STDIN_FILENO);
    } else {
        int fd = open(filename, O_RDONLY);
        check(fd >= 0, "Couldn't open file: %s", filename);
        input = readfile(fd);
    }
    match_t *m = match(g, input, pattern);
    if (m != NULL && m->end > m->start + 1) {
        if (filename != NULL) {
            if (isatty(STDOUT_FILENO)) printf("\033[1;4;33m%s\033[0m\n", filename);
            else printf("%s\n", filename);
        }
        print_match(m, isatty(STDOUT_FILENO) ? "\033[0m" : NULL, verbose);
        freefile(input);
        return 0;
    } else {
        freefile(input);
        return 1;
    }
}

#define FLAG(f) (flag=getflag((f), argv, &i))

int main(int argc, char *argv[])
{
    int verbose = 0;
    char *flag = NULL;
    char path[PATH_MAX] = {0};
    const char *rule = "find-all";

    grammar_t *g = new_grammar();

    // Load builtins:
    int fd;
    if ((fd=open("/etc/xdg/bpeg/builtins.bpeg", O_RDONLY)) >= 0)
        load_grammar(g, readfile(fd)); // Keep in memory for debugging output
    sprintf(path, "%s/.config/bpeg/builtins.bpeg", getenv("HOME"));
    if ((fd=open(path, O_RDONLY)) >= 0)
        load_grammar(g, readfile(fd)); // Keep in memory for debugging output

    int i, npatterns = 0;
    for (i = 1; i < argc; i++) {
        if (streq(argv[i], "--")) {
            ++i;
            break;
        } else if (streq(argv[i], "--help") || streq(argv[i], "-h")) {
            printf("%s\n", usage);
            return 0;
        } else if (streq(argv[i], "--verbose") || streq(argv[i], "-v")) {
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
        } else if (FLAG("--define-string") || FLAG("-D")) {
            char *def = flag;
            char *eq = strchr(def, '=');
            check(eq, usage);
            *eq = '\0';
            char *src = ++eq;
            vm_op_t *pat = bpeg_stringpattern(src);
            check(pat, "Failed to compile pattern");
            add_def(g, src, def, pat);
        } else if (FLAG("--pattern") || FLAG("-p")) {
            check(npatterns == 0, "Cannot define multiple patterns");
            vm_op_t *p = bpeg_pattern(flag);
            check(p, "Pattern failed to compile: '%s'", flag);
            add_def(g, flag, "pattern", p);
            ++npatterns;
        } else if (FLAG("--pattern-string") || FLAG("-P")) {
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

    int ret = 0;
    if (i < argc) {
        // Files pass in as command line args:
        for (int nfiles = 0; i < argc; nfiles++, i++) {
            ret |= run_match(g, argv[i], pattern, verbose);
        }
    } else if (isatty(STDIN_FILENO)) {
        // No files, no piped in input, so use * **/*:
        glob_t globbuf;
        glob("*", 0, NULL, &globbuf);
        glob("**/*", GLOB_APPEND, NULL, &globbuf);
        for (size_t i = 0; i < globbuf.gl_pathc; i++) {
            ret |= run_match(g, globbuf.gl_pathv[i], pattern, verbose);
        }
        globfree(&globbuf);
    } else {
        // Piped in input:
        ret |= run_match(g, NULL, pattern, verbose);
    }


    return ret;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
