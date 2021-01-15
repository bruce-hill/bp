//
// bp.c - Source code for the bp parser
//
// See `man ./bp.1` for more details
//
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
#include "printing.h"
#include "utils.h"
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
    " -c --context <context>           set number of lines of context to print (all: the whole file, 0: only the match, 1: the line, N: N lines of context)\n"
    " -g --grammar <grammar file>      use the specified file as a grammar\n");

// Flag-configurable options:
#define USE_DEFAULT_CONTEXT -2
#define ALL_CONTEXT -1
static int context_lines = USE_DEFAULT_CONTEXT;
static unsigned int print_color = 0;
static unsigned int print_line_numbers = 0;
static unsigned int ignorecase = 0;
static unsigned int verbose = 0;
static enum {
    MODE_NORMAL,
    MODE_LISTFILES,
    MODE_INPLACE,
    MODE_JSON,
    MODE_EXPLAIN,
} mode = MODE_NORMAL;

__attribute__((nonnull))
static char *getflag(const char *flag, char *argv[], int *i);

//
// Return a pointer to the value part of a flag, if present, otherwise NULL.
// This works for --foo=value or --foo value
//
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

//
// Scan the first few dozen bytes of a file and return 1 if the contents all
// look like printable text characters, otherwise return 0.
//
static int is_text_file(const char *filename)
{
    int fd = open(filename, O_RDONLY);
    if (fd < 0) return 0;
    unsigned char buf[64];
    int len = read(fd, buf, sizeof(buf)/sizeof(unsigned char));
    if (len < 0) return 0;
    (void)close(fd);

    for (int i = 0; i < len; i++) {
        if (!(buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r'
            || buf[i] >= '\x20'))
            return 0;
    }
    return 1;
}

//
// Print matches in JSON format.
//
static int print_matches_as_json(def_t *defs, file_t *f, vm_op_t *pattern)
{
    int matches = 0;
    for (match_t *m = NULL; (m = next_match(defs, f, m, pattern, ignorecase)); ) {
        if (++matches > 1)
            printf(",\n");
        printf("{\"filename\":\"%s\",", f->filename);
        printf("\"tree\":{\"rule\":\"text\",\"start\":%d,\"end\":%ld,\"children\":[",
               0, f->end - f->contents);
        json_match(f->contents, m, verbose);
        printf("]}}\n");
    }
    return matches;
}

//
// Print matches in a visual explanation style
//
static int explain_matches(def_t *defs, file_t *f, vm_op_t *pattern)
{
    int matches = 0;
    for (match_t *m = NULL; (m = next_match(defs, f, m, pattern, ignorecase)); ) {
        if (++matches == 1) {
            if (print_color)
                printf("\033[0;1;4;33m%s\033[0m\n", f->filename);
            else
                printf("%s:\n", f->filename);
        } else {
            printf("\n\n");
        }
        visualize_match(m);
    }
    return matches;
}

//
// Replace a file's contents with the text version of a match.
// (Useful for replacements)
//
static int inplace_modify_file(def_t *defs, file_t *f, vm_op_t *pattern)
{
    // Need to do this before matching:
    intern_file(f);

    printer_t pr = {
        .file = f,
        .context_lines = context_lines,
        .use_color = 0,
        .print_line_numbers = 0,
    };

    FILE *inplace_file = NULL; // Lazy-open this on the first match
    int matches = 0;
    for (match_t *m = NULL; (m = next_match(defs, f, m, pattern, ignorecase)); ) {
        ++matches;
        if (print_errors(&pr, m) > 0)
            exit(1);
        // Lazy-open file for writing upon first match:
        if (inplace_file == NULL) {
            inplace_file = fopen(f->filename, "w");
            check(inplace_file, "Could not open file for writing: %s\n", f->filename);
        }
        print_match(inplace_file, &pr, m);
    }

    if (inplace_file) {
        printf("%s\n", f->filename);
        fclose(inplace_file);
    }
    return matches;
}

//
// Print all the matches in a file.
//
static int print_matches(def_t *defs, file_t *f, vm_op_t *pattern)
{
    static int printed_filenames = 0;
    int matches = 0;
    printer_t pr = {
        .file = f,
        .context_lines = context_lines,
        .use_color = print_color,
        .print_line_numbers = print_line_numbers,
    };

    for (match_t *m = NULL; (m = next_match(defs, f, m, pattern, ignorecase)); ) {
        if (print_errors(&pr, m) > 0)
            exit(1);

        if (++matches == 1) {
            if (printed_filenames++ > 0) printf("\n");
            if (print_color)
                printf("\033[0;1;4;33m%s\033[0m\n", f->filename);
            else
                printf("%s:\n", f->filename);
        }
        print_match(stdout, &pr, m);
    }

    if (matches > 0) {
        // Print trailing context lines:
        print_match(stdout, &pr, NULL);
        // Ensure a trailing newline:
        if (pr.pos > f->contents && pr.pos[-1] != '\n') printf("\n");
    }

    return matches;
}

//
// For a given filename, open the file and attempt to match the given pattern
// against it, printing any results according to the flags.
//
static int process_file(def_t *defs, const char *filename, vm_op_t *pattern)
{
    file_t *f = load_file(NULL, filename);
    check(f, "Could not open file: %s", filename);

    int matches = 0;
    if (mode == MODE_EXPLAIN) {
        matches += explain_matches(defs, f, pattern);
    } else if (mode == MODE_LISTFILES) {
        match_t *m = next_match(defs, f, NULL, pattern, ignorecase);
        if (m) {
            recycle_if_unused(&m);
            printf("%s\n", f->filename);
            matches += 1;
        }
    } else if (mode == MODE_JSON) {
        matches += print_matches_as_json(defs, f, pattern);
    } else if (mode == MODE_INPLACE) {
        matches += inplace_modify_file(defs, f, pattern);
    } else {
        matches += print_matches(defs, f, pattern);
    }

#ifdef DEBUG_HEAP
    check(recycle_all_matches() == 0, "Memory leak: there should no longer be any matches in use at this point.");
#endif
    destroy_file(&f);
    fflush(stdout);
    return matches;
}

#define FLAG(f) (flag=getflag((f), argv, &i))

int main(int argc, char *argv[])
{
    char *flag = NULL;

    def_t *defs = NULL;
    file_t *loaded_files = NULL;

    // Define an opcode that is just a reference to the rule `pattern`
    file_t *pat_file = spoof_file(&loaded_files, "<pattern>", "pattern");
    vm_op_t *pattern = bp_pattern(loaded_files, pat_file->contents);

    // Define an opcode that is just a reference to the rule `replacement`
    file_t *rep_file = spoof_file(&loaded_files, "<replacement>", "replacement");
    vm_op_t *replacement = bp_pattern(rep_file, rep_file->contents);

    // Load builtins:
    file_t *xdg_file = load_file(&loaded_files, "/etc/xdg/bp/builtins.bp");
    if (xdg_file) defs = load_grammar(defs, xdg_file);
    file_t *local_file = load_file(&loaded_files, "%s/.config/bp/builtins.bp", getenv("HOME"));
    if (local_file) defs = load_grammar(defs, local_file);

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
            verbose = 1;
        } else if (streq(argv[i], "--explain")) {
            mode = MODE_EXPLAIN;
        } else if (streq(argv[i], "--json")) {
            mode = MODE_JSON;
        } else if (streq(argv[i], "--inplace")) {
            mode = MODE_INPLACE;
            context_lines = ALL_CONTEXT;
        } else if (streq(argv[i], "--ignore-case")) {
            ignorecase = 1;
        } else if (streq(argv[i], "--list-files")) {
            mode = MODE_LISTFILES;
        } else if (FLAG("--replace") || FLAG("-r")) {
            // TODO: spoof file as sprintf("pattern => '%s'", flag)
            // except that would require handling edge cases like quotation marks etc.
            file_t *replace_file = spoof_file(&loaded_files, "<replace argument>", flag);
            vm_op_t *rep = bp_replacement(replace_file, pattern, replace_file->contents);
            check(rep, "Replacement failed to compile: %s", flag);
            defs = with_def(defs, replace_file, strlen("replacement"), "replacement", rep);
            pattern = replacement;
            if (context_lines == USE_DEFAULT_CONTEXT)
                context_lines = ALL_CONTEXT;
        } else if (FLAG("--grammar") || FLAG("-g")) {
            file_t *f = load_file(&loaded_files, flag);
            if (f == NULL)
                f = load_file(&loaded_files, "%s/.config/bp/%s.bp", getenv("HOME"), flag);
            if (f == NULL)
                f = load_file(&loaded_files, "/etc/xdg/bp/%s.bp", flag);
            check(f != NULL, "Couldn't find grammar: %s", flag);
            defs = load_grammar(defs, f); // Keep in memory for debug output
        } else if (FLAG("--define") || FLAG("-d")) {
            char *def = flag;
            char *eq = strchr(def, ':');
            check(eq, "Rule definitions must include an ':'\n\n%s", usage);
            *eq = '\0';
            char *src = ++eq;
            file_t *def_file = spoof_file(&loaded_files, def, src);
            vm_op_t *pat = bp_pattern(def_file, def_file->contents);
            check(pat, "Failed to compile pattern: %s", flag);
            defs = with_def(defs, def_file, strlen(def), def, pat);
        } else if (FLAG("--define-string") || FLAG("-D")) {
            char *def = flag;
            char *eq = strchr(def, ':');
            check(eq, "Rule definitions must include an ':'\n\n%s", usage);
            *eq = '\0';
            char *src = ++eq;
            file_t *def_file = spoof_file(&loaded_files, def, src);
            vm_op_t *pat = bp_stringpattern(def_file, def_file->contents);
            check(pat, "Failed to compile pattern: %s", src);
            defs = with_def(defs, def_file, strlen(def), def, pat);
        } else if (FLAG("--pattern") || FLAG("-p")) {
            check(npatterns == 0, "Cannot define multiple patterns");
            file_t *arg_file = spoof_file(&loaded_files, "<pattern argument>", flag);
            vm_op_t *p = bp_pattern(arg_file, arg_file->contents);
            check(p, "Pattern failed to compile: %s", flag);
            defs = with_def(defs, arg_file, strlen("pattern"), "pattern", p);
            ++npatterns;
        } else if (FLAG("--pattern-string") || FLAG("-P")) {
            file_t *arg_file = spoof_file(&loaded_files, "<pattern argument>", flag);
            vm_op_t *p = bp_stringpattern(arg_file, arg_file->contents);
            check(p, "Pattern failed to compile: %s", flag);
            defs = with_def(defs, arg_file, strlen("pattern"), "pattern", p);
            ++npatterns;
        } else if (FLAG("--context") || FLAG("-c")) {
            if (streq(flag, "all"))
                context_lines = ALL_CONTEXT;
            else if (streq(flag, "none"))
                context_lines = 0;
            else
                context_lines = (int)strtol(flag, NULL, 10);
        } else if (argv[i][0] == '-' && argv[i][1] && argv[i][1] != '-') { // single-char flags
            for (char *c = &argv[i][1]; *c; ++c) {
                switch (*c) {
                    case 'h': goto flag_help; // -h
                    case 'v': verbose = 1; break; // -v
                    case 'e': mode = MODE_EXPLAIN; break; // -e
                    case 'j': mode = MODE_JSON; break; // -j
                    case 'I': mode = MODE_INPLACE; break; // -I
                    case 'i': ignorecase = 1; break; // -i
                    case 'l': mode = MODE_LISTFILES; break; // -l
                    default:
                        printf("Unrecognized flag: -%c\n\n%s\n", *c, usage);
                        return 1;
                }
            }
        } else if (argv[i][0] != '-') {
            if (npatterns > 0) break;
            // TODO: spoof file with quotation marks for better debugging
            file_t *arg_file = spoof_file(&loaded_files, "<pattern argument>", argv[i]);
            vm_op_t *p = bp_stringpattern(arg_file, arg_file->contents);
            check(p, "Pattern failed to compile: %s", argv[i]);
            defs = with_def(defs, arg_file, strlen("pattern"), "pattern", p);
            ++npatterns;
        } else {
            printf("Unrecognized flag: %s\n\n%s\n", argv[i], usage);
            return 1;
        }
    }

    if (mode == MODE_INPLACE) context_lines = ALL_CONTEXT;
    if (context_lines == USE_DEFAULT_CONTEXT) context_lines = 1;
    if (context_lines < 0 && context_lines != ALL_CONTEXT) context_lines = 0;

    if (isatty(STDOUT_FILENO)) {
        print_color = 1;
        print_line_numbers = 1;
    }

    int found = 0;
    if (mode == MODE_JSON) printf("[");
    if (i < argc) {
        // Files pass in as command line args:
        for (int nfiles = 0; i < argc; nfiles++, i++) {
            found += process_file(defs, argv[i], pattern);
        }
    } else if (isatty(STDIN_FILENO)) {
        // No files, no piped in input, so use * **/*:
        glob_t globbuf;
        glob("*", 0, NULL, &globbuf);
        glob("**/*", GLOB_APPEND, NULL, &globbuf);
        for (size_t i = 0; i < globbuf.gl_pathc; i++) {
            if (is_text_file(globbuf.gl_pathv[i]))
                found += process_file(defs, globbuf.gl_pathv[i], pattern);
        }
        globfree(&globbuf);
    } else {
        // Piped in input:
        found += process_file(defs, NULL, pattern);
    }
    if (mode == MODE_JSON) printf("]\n");

#ifdef DEBUG_HEAP
    // This code frees up all residual heap-allocated memory. Since the program
    // is about to exit, this step is unnecessary. However, it is useful for
    // tracking down memory leaks.
    free_defs(&defs, NULL);
    while (loaded_files) {
        file_t *next = loaded_files->next;
        destroy_file(&loaded_files);
        loaded_files = next;
    }
    free_all_matches();
#endif

    return (found > 0) ? 0 : 1;
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
