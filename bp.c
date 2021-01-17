//
// bp.c - Source code for the bp parser
//
// See `man ./bp.1` for more details
//
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "definitions.h"
#include "files.h"
#include "json.h"
#include "match.h"
#include "pattern.h"
#include "print.h"
#include "utils.h"

#ifndef BP_NAME
#define BP_NAME "bp"
#endif

static const char *usage = (
    BP_NAME" - a Parsing Expression Grammar command line tool\n\n"
    "Usage:\n"
    "  "BP_NAME" [flags] <pattern> [<input files>...]\n\n"
    "Flags:\n"
    " -h --help                        print the usage and quit\n"
    " -v --verbose                     print verbose debugging info\n"
    " -e --explain                     explain the matches\n"
    " -j --json                        print matches as a list of JSON objects\n"
    " -i --ignore-case                 preform matching case-insensitively\n"
    " -I --inplace                     modify a file in-place\n"
    " -C --confirm                     ask for confirmation on each replacement\n"
    " -l --list-files                  list filenames only\n"
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
typedef enum { CONFIRM_ASK, CONFIRM_ALL, CONFIRM_NONE } confirm_t;
static confirm_t confirm = CONFIRM_ALL;
static enum {
    MODE_NORMAL,
    MODE_LISTFILES,
    MODE_INPLACE,
    MODE_JSON,
    MODE_EXPLAIN,
} mode = MODE_NORMAL;

// If a filename is put here, it will be deleted if a signal is received
const char *in_use_tempfile = NULL;

// Used for user input/output that doesn't interfere with unix pipeline
FILE *tty_out = NULL, *tty_in = NULL;

//
// Helper function to reduce code duplication
//
static inline void fprint_filename(FILE *out, const char *filename)
{
    if (!filename[0]) return;
    if (print_color) fprintf(out, "\033[0;1;4;33m%s\033[0m\n", filename);
    else fprintf(out, "%s:\n", filename);
}

//
// Return a pointer to the value part of a flag, if present, otherwise NULL.
// This works for --foo=value or --foo value
//
__attribute__((nonnull))
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
// Return whether or not a boolean flag exists, and update i/argv to move past
// it if it does.
//
__attribute__((nonnull))
static int boolflag(const char *flag, char *argv[], int *i)
{
    check(argv[*i], "Attempt to get flag from NULL argument");
    if (streq(argv[*i], flag)) return 1;
    if (flag[0] == '-' && flag[1] != '-' && flag[2] == '\0' && argv[*i][0] == '-' && argv[*i][1] != '-') {
        char *p = strchr(argv[*i], flag[1]);
        if (p) {
            --(*i); // Recheck this flag
            memmove(p, p+1, strlen(p+1)+1);
            return 1;
        }
    }
    return 0;
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
static int print_matches_as_json(def_t *defs, file_t *f, pat_t *pattern)
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
static int explain_matches(def_t *defs, file_t *f, pat_t *pattern)
{
    int matches = 0;
    for (match_t *m = NULL; (m = next_match(defs, f, m, pattern, ignorecase)); ) {
        if (++matches == 1) {
            fprint_filename(stdout, f->filename);
        } else {
            printf("\n\n");
        }
        visualize_match(m);
    }
    return matches;
}

//
// Cleanup function to ensure no temp files are left around if the program
// exits unexpectedly.
//
static void cleanup(void)
{
    if (in_use_tempfile) {
        remove(in_use_tempfile);
        in_use_tempfile = NULL;
    }
}

//
// Signal handler to ensure cleanup happens.
//
static void sig_handler(int sig) { (void)sig; cleanup(); }

//
// Present the user with a prompt to confirm replacements before they happen.
// If the user rejects a replacement, the match object is set to the underlying
// non-replacement value.
//
static void confirm_replacements(file_t *f, match_t *m, confirm_t *confirm)
{
    if (*confirm == CONFIRM_ALL) return;
    if (m->pat->type == BP_REPLACE) {
        if (*confirm == CONFIRM_NONE) {
            m->skip_replacement = 1;
            goto check_children;
        }

        { // Print the original
            printer_t pr = {.file = f, .context_lines = context_lines,
                .use_color = 1, .print_line_numbers = 1};
            print_match(tty_out, &pr, m->child);
            // Print trailing context lines:
            print_match(tty_out, &pr, NULL);
        }
        if (context_lines > 1) fprintf(tty_out, "\n");
        { // Print the replacement
            printer_t pr = {.file = f, .context_lines = context_lines,
                .use_color = 1, .print_line_numbers = 1};
            print_match(tty_out, &pr, m);
            // Print trailing context lines:
            print_match(tty_out, &pr, NULL);
        }

      retry:
        fprintf(tty_out, "\033[1mReplace? (y)es (n)o (r)emaining (d)one\033[0m ");
        fflush(tty_out);

        char *answer = NULL;
        size_t len = 0;
        if (getline(&answer, &len, tty_in) > 0) {
            if (strlen(answer) > 2) goto retry;
            switch (answer[0]) {
                case 'y': case '\n': break;
                case 'n': m->skip_replacement = 1; break;
                case 'r': *confirm = CONFIRM_ALL; break;
                case 'd': m->skip_replacement = 1; *confirm = CONFIRM_NONE; break;
                default: goto retry;
            }
        }
        if (answer) xfree(&answer);
        fprintf(tty_out, "\n");
    }

  check_children:
    if (m->child)
        confirm_replacements(f, m->child, confirm);
    if (m->nextsibling)
        confirm_replacements(f, m->nextsibling, confirm);
}

//
// Replace a file's contents with the text version of a match.
// (Useful for replacements)
//
static int inplace_modify_file(def_t *defs, file_t *f, pat_t *pattern)
{
    char tmp_filename[PATH_MAX+1] = {0};
    printer_t pr = {
        .file = f,
        .context_lines = ALL_CONTEXT,
        .use_color = 0,
        .print_line_numbers = 0,
    };

    FILE *inplace_file = NULL; // Lazy-open this on the first match
    int matches = 0;
    confirm_t confirm_file = confirm;
    for (match_t *m = NULL; (m = next_match(defs, f, m, pattern, ignorecase)); ) {
        ++matches;
        if (print_errors(&pr, m) > 0)
            exit(1);
        // Lazy-open file for writing upon first match:
        if (inplace_file == NULL) {
            check(snprintf(tmp_filename, PATH_MAX, "%s.tmp.XXXXXX", f->filename) <= PATH_MAX,
                "Failed to build temporary file template");
            int out_fd = mkstemp(tmp_filename);
            check(out_fd >= 0, "Failed to create temporary inplace file");
            in_use_tempfile = tmp_filename;
            inplace_file = fdopen(out_fd, "w");
            if (confirm == CONFIRM_ASK && f->filename)
                fprint_filename(tty_out, f->filename);
        }
        confirm_replacements(f, m, &confirm_file);
        if (!in_use_tempfile) { // signal interrupted, so abort
            fclose(inplace_file);
            exit(1);
        }
        print_match(inplace_file, &pr, m);
    }

    if (inplace_file) {
        // Print trailing context lines:
        print_match(inplace_file, &pr, NULL);
        if (confirm == CONFIRM_ALL)
            printf("%s\n", f->filename);
        fclose(inplace_file);

        // TODO: if I want to implement backup files then add a line like this:
        // if (backup) rename(f->filename, f->filename + ".bak");
        check(rename(tmp_filename, f->filename) == 0,
              "Failed to write file replacement for %s", f->filename);

        in_use_tempfile = NULL;
    }

    return matches;
}

//
// Print all the matches in a file.
//
static int print_matches(def_t *defs, file_t *f, pat_t *pattern)
{
    static int printed_filenames = 0;
    int matches = 0;
    printer_t pr = {
        .file = f,
        .context_lines = context_lines,
        .use_color = print_color,
        .print_line_numbers = print_line_numbers,
    };

    confirm_t confirm_file = confirm;
    for (match_t *m = NULL; (m = next_match(defs, f, m, pattern, ignorecase)); ) {
        if (print_errors(&pr, m) > 0)
            exit(1);

        if (++matches == 1) {
            if (printed_filenames++ > 0) printf("\n");
            fprint_filename(stdout, f->filename);
        }
        confirm_replacements(f, m, &confirm_file);
        print_match(stdout, &pr, m);
    }

    if (matches > 0) {
        // Print trailing context lines:
        print_match(stdout, &pr, NULL);
    }

    return matches;
}

//
// For a given filename, open the file and attempt to match the given pattern
// against it, printing any results according to the flags.
//
static int process_file(def_t *defs, const char *filename, pat_t *pattern)
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
#define BOOLFLAG(f) (boolflag((f), argv, &i))

int main(int argc, char *argv[])
{
    char *flag = NULL;

    def_t *defs = NULL;
    file_t *loaded_files = NULL;

    // Define a pattern that is just a reference to the rule `pattern`
    file_t *pat_file = spoof_file(&loaded_files, "<pattern>", "pattern");
    pat_t *pattern = bp_pattern(loaded_files, pat_file->contents);

    // Define a pattern that is just a reference to the rule `replacement`
    file_t *rep_file = spoof_file(&loaded_files, "<replacement>", "replacement");
    pat_t *replacement = bp_pattern(rep_file, rep_file->contents);

    // Load builtins:
    file_t *xdg_file = load_file(&loaded_files, "/etc/xdg/"BP_NAME"/builtins.bp");
    if (xdg_file) defs = load_grammar(defs, xdg_file);
    file_t *local_file = load_file(&loaded_files, "%s/.config/"BP_NAME"/builtins.bp", getenv("HOME"));
    if (local_file) defs = load_grammar(defs, local_file);

    check(argc > 1, "%s", usage);
    int i, npatterns = 0, git = 0;
    for (i = 1; i < argc; i++) {
        if (streq(argv[i], "--")) {
            ++i;
            break;
        } else if (BOOLFLAG("-h") || BOOLFLAG("--help")) {
            printf("%s\n", usage);
            return 0;
        } else if (BOOLFLAG("-v") || BOOLFLAG("--verbose")) {
            verbose = 1;
        } else if (BOOLFLAG("-e") || BOOLFLAG("--explain")) {
            mode = MODE_EXPLAIN;
        } else if (BOOLFLAG("-j") || BOOLFLAG("--json")) {
            mode = MODE_JSON;
        } else if (BOOLFLAG("-I") || BOOLFLAG("--inplace")) {
            mode = MODE_INPLACE;
        } else if (BOOLFLAG("-C") || BOOLFLAG("--confirm")) {
            confirm = CONFIRM_ASK;
        } else if (BOOLFLAG("-G") || BOOLFLAG("--git")) {
            git = 1;
        } else if (BOOLFLAG("-i") || BOOLFLAG("--ignore-case")) {
            ignorecase = 1;
        } else if (BOOLFLAG("-l") || BOOLFLAG("--list-files")) {
            mode = MODE_LISTFILES;
        } else if (FLAG("-r")     || FLAG("--replace")) {
            // TODO: spoof file as sprintf("pattern => '%s'", flag)
            // except that would require handling edge cases like quotation marks etc.
            file_t *replace_file = spoof_file(&loaded_files, "<replace argument>", flag);
            pat_t *rep = bp_replacement(replace_file, pattern, replace_file->contents);
            check(rep, "Replacement failed to compile: %s", flag);
            defs = with_def(defs, replace_file, strlen("replacement"), "replacement", rep);
            pattern = replacement;
        } else if (FLAG("-g")     || FLAG("--grammar")) {
            file_t *f = load_file(&loaded_files, flag);
            if (f == NULL)
                f = load_file(&loaded_files, "%s/.config/"BP_NAME"/%s.bp", getenv("HOME"), flag);
            if (f == NULL)
                f = load_file(&loaded_files, "/etc/xdg/"BP_NAME"/%s.bp", flag);
            check(f != NULL, "Couldn't find grammar: %s", flag);
            defs = load_grammar(defs, f); // Keep in memory for debug output
        } else if (FLAG("-p")     || FLAG("--pattern")) {
            file_t *arg_file = spoof_file(&loaded_files, "<pattern argument>", flag);
            for (const char *str = arg_file->contents; str < arg_file->end; ) {
                def_t *d = bp_definition(arg_file, str);
                if (d) {
                    d->next = defs;
                    defs = d;
                    str = d->pat->end;
                } else {
                    pat_t *p = bp_pattern(arg_file, str);
                    if (!p) {
                        fprint_line(stdout, arg_file, str, arg_file->end,
                                    "Failed to compile this part of the argument");
                        return 1;
                    }
                    check(npatterns == 0, "Cannot define multiple patterns");
                    defs = with_def(defs, arg_file, strlen("pattern"), "pattern", p);
                    ++npatterns;
                    str = p->end;
                }
                str = after_spaces(str);
                str = strchr(str, ';') ? strchr(str, ';') + 1 : str;
            }
        } else if (FLAG("-P")     || FLAG("--pattern-string")) {
            file_t *arg_file = spoof_file(&loaded_files, "<pattern argument>", flag);
            pat_t *p = bp_stringpattern(arg_file, arg_file->contents);
            check(p, "Pattern failed to compile: %s", flag);
            defs = with_def(defs, arg_file, strlen("pattern"), "pattern", p);
            ++npatterns;
        } else if (FLAG("-c")     || FLAG("--context")) {
            if (streq(flag, "all"))
                context_lines = ALL_CONTEXT;
            else if (streq(flag, "none"))
                context_lines = 0;
            else
                context_lines = (int)strtol(flag, NULL, 10);
        } else if (argv[i][0] == '-' && argv[i][1] && argv[i][1] != '-') { // single-char flags
            printf("Unrecognized flag: -%c\n\n%s\n", argv[i][1], usage);
            return 1;
        } else if (argv[i][0] != '-') {
            if (npatterns > 0) break;
            // TODO: spoof file with quotation marks for better debugging
            file_t *arg_file = spoof_file(&loaded_files, "<pattern argument>", argv[i]);
            pat_t *p = bp_stringpattern(arg_file, arg_file->contents);
            check(p, "Pattern failed to compile: %s", argv[i]);
            defs = with_def(defs, arg_file, strlen("pattern"), "pattern", p);
            ++npatterns;
        } else {
            printf("Unrecognized flag: %s\n\n%s\n", argv[i], usage);
            return 1;
        }
    }

    if (context_lines == USE_DEFAULT_CONTEXT) context_lines = 1;
    if (context_lines < 0 && context_lines != ALL_CONTEXT) context_lines = 0;

    if (isatty(STDOUT_FILENO)) {
        print_color = 1;
        print_line_numbers = 1;
    }

    // If any of these signals triggers, and there is a temporary file in use,
    // be sure to clean it up before exiting.
    int signals[] = {SIGTERM, SIGINT, SIGXCPU, SIGXFSZ, SIGVTALRM, SIGPROF, SIGSEGV, SIGTSTP};
    struct sigaction sa = {.sa_handler = &sig_handler, .sa_flags = (int)(SA_NODEFER | SA_RESETHAND)};
    for (size_t i = 0; i < sizeof(signals)/sizeof(signals[0]); i++)
        sigaction(signals[i], &sa, NULL);

    // Handle exit() calls gracefully:
    atexit(&cleanup);

    // User input/output is handled through /dev/tty so that normal unix pipes
    // can work properly while simultaneously asking for user input.
    if (confirm == CONFIRM_ASK) {
        tty_in = fopen("/dev/tty", "r");
        tty_out = fopen("/dev/tty", "w");
    }

    int found = 0;
    if (mode == MODE_JSON) printf("[");
    if (git) {
        int fds[2];
        check(pipe(fds) == 0, "Failed to create pipe");
        pid_t child = fork();
        check(child != -1, "Failed to fork");
        if (child == 0) {
            char **git_args = calloc((size_t)(2+(argc-i)+1), sizeof(char*));
            int g = 0;
            git_args[g++] = "git";
            git_args[g++] = "ls-files";
            while (i < argc) git_args[g++] = argv[i++];
            dup2(fds[STDOUT_FILENO], STDOUT_FILENO);
            close(fds[STDIN_FILENO]);
            execvp("git", git_args);
            _exit(1);
        }
        close(fds[STDOUT_FILENO]);
        char path[PATH_MAX+2] = {0}; // path + \n + \0
        while (read(fds[STDIN_FILENO], path, PATH_MAX+1) > 0) { // Iterate over chunks
            for (char *nl; (nl = strchr(path, '\n')); ) { // Iterate over nl-terminated lines
                *nl = '\0';
                found += process_file(defs, path, pattern);
                memmove(path, nl+1, sizeof(path)-(size_t)(nl+1-path));
            }
        }
        close(fds[STDIN_FILENO]);
        int status;
        waitpid(child, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "`git --ls-files` failed. Do you have git installed?");
    } else if (i < argc) {
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

    if (tty_out) fclose(tty_out);
    if (tty_in) fclose(tty_in);

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
