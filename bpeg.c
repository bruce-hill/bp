/*
 * bpeg.c - Source code for the bpeg parser
 *
 * See `man ./bp.1` for more details
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
#include "json.h"
#include "utils.h"
#include "viz.h"
#include "vm.h"

static const char *usage = (
    "BP - a Parsing Expression Grammar command line tool\n\n"
    "Usage:\n"
    "  bp [flags] <pattern> [<input files>...]\n\n"
    "Flags:\n"
    " -h --help                        print the usage and quit\n"
    " -v --verbose                     print verbose debugging info\n"
    " -e --explain                     explain the matches\n"
    " -j --json                        print matches as a list of JSON objects\n"
    " -I --inplace                     modify a file in-place\n"
    " -i --ignore-case                 preform matching case-insensitively\n"
    " -l --list-files                  list filenames only\n"
    " -d --define <name>:<def>         define a grammar rule\n"
    " -D --define-string <name>:<def>  define a grammar rule (string-pattern)\n"
    " -p --pattern <pat>               provide a pattern (equivalent to bp '\\(<pat>)')\n"
    " -P --pattern-string <pat>        provide a string pattern (may be useful if '<pat>' begins with a '-')\n"
    " -r --replace <replacement>       replace the input pattern with the given replacement\n"
    " -m --mode <mode>                 set the behavior mode (defult: find-all)\n"
    " -g --grammar <grammar file>      use the specified file as a grammar\n");

static print_options_t print_options = 0;

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
    if (m->op->op == VM_CAPTURE && m->op->args.capture.name && streq(m->op->args.capture.name, "!")) {
        printf("\033[31;1m");
        print_match(stdout, f, m, print_options);
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
    static int printed_matches = 0;
    file_t *f = load_file(filename);
    check(f, "Could not open file: %s", filename);
    if (flags & BPEG_INPLACE) // Need to do this before matching
        intern_file(f);
    match_t *m = match(g, f, f->contents, pattern, flags);
    if (m && print_errors(f, m) > 0)
        _exit(1);
    if (m != NULL && m->end > m->start + 1) {
        ++printed_matches;

        if (flags & BPEG_EXPLAIN) {
            if (filename)
                printf("\033[1;4m%s\033[0m\n", filename);
            visualize_match(m);
        } else if (flags & BPEG_LISTFILES) {
            printf("%s\n", filename);
        } else if (flags & BPEG_JSON) {
            if (printed_matches > 1)
                printf(",\n");
            printf("{\"filename\":\"%s\",", filename ? filename : "-");
            printf("\"tree\":{\"rule\":\"text\",\"start\":%d,\"end\":%ld,\"children\":[",
                   0, f->end - f->contents);
            json_match(f->contents, m, (flags & BPEG_VERBOSE) ? 1 : 0);
            printf("]}}\n");
        } else if (flags & BPEG_INPLACE) {
            FILE *out = fopen(filename, "w");
            print_match(out, f, m, 0);
            fclose(out);
        } else {
            if (printed_matches > 1)
                fputc('\n', stdout);
            if (filename) {
                if (print_options & PRINT_COLOR)
                    printf("\033[1;4;33m%s\033[0m\n", filename);
                else
                    printf("%s:\n", filename);
            }
            print_match(stdout, f, m, print_options);
        }
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
    if (access("/etc/xdg/bp/builtins.bp", R_OK) != -1)
        load_grammar(g, load_file("/etc/xdg/bp/builtins.bp")); // Keep in memory for debugging output
    sprintf(path, "%s/.config/bp/builtins.bp", getenv("HOME"));
    if (access(path, R_OK) != -1)
        load_grammar(g, load_file(path)); // Keep in memory for debugging output

    int i, npatterns = 0;
    check(argc > 1, "%s", usage);
    for (i = 1; i < argc; i++) {
        if (streq(argv[i], "--")) {
            ++i;
            break;
        } else if (streq(argv[i], "--help")) {
          flag_help:
            printf("%s\n", usage);
            return 0;
        } else if (streq(argv[i], "--verbose")) {
            flags |= BPEG_VERBOSE;
        } else if (streq(argv[i], "--explain")) {
            flags |= BPEG_EXPLAIN;
        } else if (streq(argv[i], "--json")) {
            flags |= BPEG_JSON;
        } else if (streq(argv[i], "--inplace")) {
            flags |= BPEG_INPLACE;
        } else if (streq(argv[i], "--ignore-case")) {
            flags |= BPEG_IGNORECASE;
        } else if (streq(argv[i], "--list-files")) {
            flags |= BPEG_LISTFILES;
        } else if (FLAG("--replace") || FLAG("-r")) {
            file_t *pat_file = spoof_file("<pattern>", "pattern");
            vm_op_t *patref = bpeg_pattern(pat_file, pat_file->contents);
            file_t *replace_file = spoof_file("<replace argument>", flag);
            vm_op_t *rep = bpeg_replacement(replace_file, patref, replace_file->contents);
            check(rep, "Replacement failed to compile: %s", flag);
            add_def(g, replace_file, replace_file->contents, "replacement", rep);
            rule = "replace-all";
        } else if (FLAG("--grammar") || FLAG("-g")) {
            file_t *f = load_file(flag);
            if (f == NULL) {
                sprintf(path, "%s/.config/bp/%s.bp", getenv("HOME"), flag);
                f = load_file(path);
            }
            if (f == NULL) {
                sprintf(path, "/etc/xdg/bp/%s.bp", flag);
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
            file_t *def_file = spoof_file(def, src);
            vm_op_t *pat = bpeg_pattern(def_file, def_file->contents);
            check(pat, "Failed to compile pattern: %s", flag);
            add_def(g, def_file, src, def, pat);
        } else if (FLAG("--define-string") || FLAG("-D")) {
            char *def = flag;
            char *eq = strchr(def, ':');
            check(eq, "Rule definitions must include an ':'\n\n%s", usage);
            *eq = '\0';
            char *src = ++eq;
            file_t *def_file = spoof_file(def, flag);
            vm_op_t *pat = bpeg_stringpattern(def_file, def_file->contents);
            check(pat, "Failed to compile pattern: %s", flag);
            add_def(g, def_file, src, def, pat);
        } else if (FLAG("--pattern") || FLAG("-p")) {
            check(npatterns == 0, "Cannot define multiple patterns");
            file_t *arg_file = spoof_file("<pattern argument>", flag);
            vm_op_t *p = bpeg_pattern(arg_file, arg_file->contents);
            check(p, "Pattern failed to compile: %s", flag);
            add_def(g, arg_file, flag, "pattern", p);
            ++npatterns;
        } else if (FLAG("--pattern-string") || FLAG("-P")) {
            file_t *arg_file = spoof_file("<pattern argument>", flag);
            vm_op_t *p = bpeg_stringpattern(arg_file, arg_file->contents);
            check(p, "Pattern failed to compile: %s", flag);
            add_def(g, arg_file, flag, "pattern", p);
            ++npatterns;
        } else if (FLAG("--mode") || FLAG("-m")) {
            rule = flag;
        } else if (argv[i][0] == '-' && argv[i][1] && argv[i][1] != '-') { // single-char flags
            for (char *c = &argv[i][1]; *c; ++c) {
                switch (*c) {
                    case 'h': goto flag_help; // -h
                    case 'v': flags |= BPEG_VERBOSE; break; // -v
                    case 'e': flags |= BPEG_EXPLAIN; break; // -e
                    case 'j': flags |= BPEG_JSON; break; // -j
                    case 'I': flags |= BPEG_INPLACE; break; // -I
                    case 'i': flags |= BPEG_IGNORECASE; break; // -i
                    case 'l': flags |= BPEG_LISTFILES; break; // -l
                    default:
                        printf("Unrecognized flag: -%c\n\n%s\n", *c, usage);
                        return 1;
                }
            }
        } else if (argv[i][0] != '-') {
            if (npatterns > 0) break;
            file_t *arg_file = spoof_file("<pattern argument>", argv[i]);
            vm_op_t *p = bpeg_stringpattern(arg_file, arg_file->contents);
            check(p, "Pattern failed to compile: %s", argv[i]);
            add_def(g, arg_file, argv[i], "pattern", p);
            ++npatterns;
        } else {
            printf("Unrecognized flag: %s\n\n%s\n", argv[i], usage);
            return 1;
        }
    }

    if (((flags & BPEG_JSON) != 0) + ((flags & BPEG_EXPLAIN) != 0) + ((flags & BPEG_LISTFILES) != 0) > 1) {
        printf("Please choose no more than one of the flags: -j/--json, -e/--explain, and -l/--list-files.\n"
               "They are mutually contradictory.\n");
        return 1;
    }

    if (isatty(STDOUT_FILENO)) {
        print_options |= PRINT_COLOR | PRINT_LINE_NUMBERS;
    }

    vm_op_t *pattern = lookup(g, rule);
    check(pattern != NULL, "No such rule: '%s'", rule);

    int ret = 1;
    if (flags & BPEG_JSON) printf("[");
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
    if (flags & BPEG_JSON) printf("]\n");

    return ret;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
