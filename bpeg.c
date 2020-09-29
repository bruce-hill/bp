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
#include "file_loader.h"
#include "grammar.h"
#include "utils.h"
#include "vm.h"

static const char *usage = (
    "BPEG - a Parsing Expression Grammar command line tool\n\n"
    "Usage:\n"
    "  bpeg [flags] <pattern> [<input files>...]\n\n"
    "Flags:\n"
    " -h --help                        print the usage and quit\n"
    " -v --verbose                     print verbose debugging info\n"
    " -i --ignore-case                 preform matching case-insensitively\n"
    " -d --define <name>:<def>         define a grammar rule\n"
    " -D --define-string <name>:<def>  define a grammar rule (string-pattern)\n"
    " -p --pattern <pat>               provide a pattern (equivalent to bpeg '\\(<pat>)')\n"
    " -P --pattern-string <pat>        provide a string pattern (may be useful if '<pat>' begins with a '-')\n"
    " -r --replace <replacement>       replace the input pattern with the given replacement\n"
    " -m --mode <mode>                 set the behavior mode (defult: find-all)\n"
    " -g --grammar <grammar file>      use the specified file as a grammar\n");

static char *getflag(const char *flag, char *argv[], int *i)
{
    size_t n = strlen(flag);
    check(argv[*i], "Attempt to get flag from NULL argument");
    if (strncmp(argv[*i], flag, n) == 0) {
        if (argv[*i][n] == '=') {
            return &argv[*i][n+1];
        } else if (argv[*i][n] == '\0') {
            check(argv[*i+1], "Expected argument after '%s'\n\n%s", flag, usage);
            ++(*i);
            return argv[*i];
        }
    }
    return NULL;
}

static int print_errors(file_t *f, match_t *m)
{
    int ret = 0;
    if (m->op->op == VM_CAPTURE && m->value.name && streq(m->value.name, "!")) {
        printf("\033[31;1m");
        print_match(f, m);
        printf("\033[0m\n");
        fprint_line(stdout, f, m->start, m->end, " ");
        return 1;
    }
    if (m->child) ret += print_errors(f, m->child);
    if (m->nextsibling) ret += print_errors(f, m->nextsibling);
    return ret;
}

static int run_match(grammar_t *g, const char *filename, vm_op_t *pattern, unsigned int flags)
{
    file_t *f = load_file(filename);
    check(f, "Could not open file: %s", filename);
    match_t *m = match(g, f, f->contents, pattern, flags);
    if (m && print_errors(f, m) > 0)
        _exit(1);
    if (m != NULL && m->end > m->start + 1) {
        print_match(f, m);
        destroy_file(&f);
        return 0;
    } else {
        destroy_file(&f);
        return 1;
    }
}

#define FLAG(f) (flag=getflag((f), argv, &i))

int main(int argc, char *argv[])
{
    unsigned int flags = 0;
    char *flag = NULL;
    char path[PATH_MAX] = {0};
    const char *rule = "find-all";

    grammar_t *g = new_grammar();

    // Load builtins:
    if (access("/etc/xdg/bpeg/builtins.bpeg", R_OK) != -1)
        load_grammar(g, load_file("/etc/xdg/bpeg/builtins.bpeg")); // Keep in memory for debugging output
    sprintf(path, "%s/.config/bpeg/builtins.bpeg", getenv("HOME"));
    if (access(path, R_OK) != -1)
        load_grammar(g, load_file(path)); // Keep in memory for debugging output

    int i, npatterns = 0;
    check(argc > 1, "%s", usage);
    for (i = 1; i < argc; i++) {
        if (streq(argv[i], "--")) {
            ++i;
            break;
        } else if (streq(argv[i], "--help") || streq(argv[i], "-h")) {
            printf("%s\n", usage);
            return 0;
        } else if (streq(argv[i], "--verbose") || streq(argv[i], "-v")) {
            flags |= BPEG_VERBOSE;
        } else if (streq(argv[i], "--ignore-case") || streq(argv[i], "-i")) {
            flags |= BPEG_IGNORECASE;
        } else if (FLAG("--replace") || FLAG("-r")) {
            file_t *replace_file = spoof_file("<replace argument>", flag);
            vm_op_t *p = bpeg_replacement(bpeg_pattern(replace_file, "pattern"), flag);
            check(p, "Replacement failed to compile");
            add_def(g, replace_file, flag, "replacement", p);
            rule = "replace-all";
        } else if (FLAG("--grammar") || FLAG("-g")) {
            file_t *f = load_file(flag);
            if (f == NULL) {
                sprintf(path, "%s/.config/bpeg/%s.bpeg", getenv("HOME"), flag);
                f = load_file(path);
            }
            if (f == NULL) {
                sprintf(path, "/etc/xdg/bpeg/%s.bpeg", flag);
                f = load_file(path);
            }
            check(f != NULL, "Couldn't find grammar: %s", flag);
            load_grammar(g, f); // Keep in memory for debug output
        } else if (FLAG("--define") || FLAG("-d")) {
            char *def = flag;
            char *eq = strchr(def, ':');
            check(eq, "Rule definitions must include an ':'\n\n%s", usage);
            *eq = '\0';
            char *src = ++eq;
            file_t *def_file = spoof_file(def, flag);
            vm_op_t *pat = bpeg_pattern(def_file, src);
            check(pat, "Failed to compile pattern");
            add_def(g, def_file, src, def, pat);
        } else if (FLAG("--define-string") || FLAG("-D")) {
            char *def = flag;
            char *eq = strchr(def, ':');
            check(eq, "Rule definitions must include an ':'\n\n%s", usage);
            *eq = '\0';
            char *src = ++eq;
            file_t *def_file = spoof_file(def, flag);
            vm_op_t *pat = bpeg_stringpattern(def_file, src);
            check(pat, "Failed to compile pattern");
            add_def(g, def_file, src, def, pat);
        } else if (FLAG("--pattern") || FLAG("-p")) {
            check(npatterns == 0, "Cannot define multiple patterns");
            file_t *arg_file = spoof_file("<pattern argument>", flag);
            vm_op_t *p = bpeg_pattern(arg_file, flag);
            check(p, "Pattern failed to compile: '%s'", flag);
            add_def(g, arg_file, flag, "pattern", p);
            ++npatterns;
        } else if (FLAG("--pattern-string") || FLAG("-P")) {
            file_t *arg_file = spoof_file("<pattern argument>", flag);
            vm_op_t *p = bpeg_stringpattern(arg_file, flag);
            check(p, "Pattern failed to compile");
            add_def(g, arg_file, flag, "pattern", p);
            ++npatterns;
        } else if (FLAG("--mode") || FLAG("-m")) {
            rule = flag;
        } else if (argv[i][0] != '-') {
            if (npatterns > 0) break;
            file_t *arg_file = spoof_file("<pattern argument>", flag);
            vm_op_t *p = bpeg_stringpattern(arg_file, argv[i]);
            check(p, "Pattern failed to compile");
            add_def(g, arg_file, argv[i], "pattern", p);
            ++npatterns;
        } else {
            printf("Unrecognized flag: %s\n\n%s\n", argv[i], usage);
            return 1;
        }
    }

    if (isatty(STDOUT_FILENO)) {
        file_t *is_tty_file = spoof_file("<is-tty>", flag);
        vm_op_t *p = bpeg_pattern(is_tty_file, "''");
        check(p, "Failed to compile is-tty");
        add_def(g, is_tty_file, "''", "is-tty", p);
    }

    vm_op_t *pattern = lookup(g, rule);
    check(pattern != NULL, "No such rule: '%s'", rule);

    int ret = 1;
    if (i < argc) {
        // Files pass in as command line args:
        for (int nfiles = 0; i < argc; nfiles++, i++) {
            ret &= run_match(g, argv[i], pattern, flags);
        }
    } else if (isatty(STDIN_FILENO)) {
        // No files, no piped in input, so use * **/*:
        glob_t globbuf;
        glob("*", 0, NULL, &globbuf);
        glob("**/*", GLOB_APPEND, NULL, &globbuf);
        for (size_t i = 0; i < globbuf.gl_pathc; i++) {
            ret &= run_match(g, globbuf.gl_pathv[i], pattern, flags);
        }
        globfree(&globbuf);
    } else {
        // Piped in input:
        ret &= run_match(g, NULL, pattern, flags);
    }


    return ret;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
