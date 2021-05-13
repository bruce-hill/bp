//
// bp.c - Source code for the bp parser
//
// See `man ./bp.1` for more details
//

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static const char *description = (
    BP_NAME" - a Parsing Expression Grammar command line tool");
static const char *usage = (
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
    " -r --replace <replacement>       replace the input pattern with the given replacement\n"
    " -s --skip <skip pattern>         skip over the given pattern when looking for matches\n"
    " -c --context <context>           set number of lines of context to print (all: the whole file, 0: only the match, 1: the line, N: N lines of context)\n"
    " -f --format auto|fancy|plain     set the output format\n"
    " -g --grammar <grammar file>      use the specified file as a grammar");

// Used as a heuristic to check if a file is binary or text:
#define CHECK_FIRST_N_BYTES 128

// Flag-configurable options:
typedef enum { CONFIRM_ASK, CONFIRM_ALL, CONFIRM_NONE } confirm_t;
#define USE_DEFAULT_CONTEXT -2
#define ALL_CONTEXT -1
static struct {
    int context_lines;
    bool ignorecase, verbose, git_mode;
    confirm_t confirm;
    enum { MODE_NORMAL, MODE_LISTFILES, MODE_INPLACE, MODE_JSON, MODE_EXPLAIN } mode;
    enum { FORMAT_AUTO, FORMAT_FANCY, FORMAT_PLAIN } format;
    pat_t *skip;
} options = {
    .context_lines = USE_DEFAULT_CONTEXT,
    .ignorecase = false,
    .verbose = false,
    .confirm = CONFIRM_ALL,
    .mode = MODE_NORMAL,
    .format = FORMAT_AUTO,
    .skip = NULL,
};

// If a file is partly through being modified when the program exits, restore it from backup.
static FILE *modifying_file = NULL;
static file_t *backup_file;

// Used for user input/output that doesn't interfere with unix pipeline
static FILE *tty_out = NULL, *tty_in = NULL;

//
// Helper function to reduce code duplication
//
static inline void fprint_filename(FILE *out, const char *filename)
{
    if (!filename[0]) return;
    if (options.format == FORMAT_FANCY) fprintf(out, "\033[0;1;4;33m%s\033[0m\n", filename);
    else fprintf(out, "%s:\n", filename);
}

//
// Look for a key/value flag at the first position in the given argument list.
// If the flag is found, update `next` to point to the next place to check for a flag.
// The contents of argv[0] may be modified for single-char flags.
// Return the flag's value.
//
__attribute__((nonnull))
static char *get_flag(char *argv[], const char *flag, char ***next)
{
    size_t n = strlen(flag);
    if (strncmp(argv[0], flag, n) != 0) return NULL;
    if (argv[0][n] == '=') { // --foo=baz, -f=baz
        *next = &argv[1];
        return &argv[0][n+1];
    } else if (argv[0][n] == '\0') { // --foo baz, -f baz
        if (!argv[1])
            errx(EXIT_FAILURE, "Expected argument after '%s'\n\n%s", flag, usage);
        *next = &argv[2];
        return argv[1];
    } else if (flag[0] == '-' && flag[1] != '-' && flag[2] == '\0') { // -f...
        *next = &argv[1];
        return &argv[0][n];
    }
    return NULL;
}

//
// Look for a flag at the first position in the given argument list.
// If the flag is found, update `next` to point to the next place to check for a flag.
// The contents of argv[0] may be modified for single-char flags.
// Return a boolean for whether or not the flag was found.
//
__attribute__((nonnull))
static bool get_boolflag(char *argv[], const char *flag, char ***next)
{
    size_t n = strlen(flag);
    if (strncmp(argv[0], flag, n) != 0) return false;
    if (argv[0][n] == '\0') { // --foo, -f
        *next = &argv[1];
        return true;
    } else if (flag[0] == '-' && flag[1] != '-' && flag[2] == '\0') { // -f...
        memmove(&argv[0][1], &argv[0][2], 1+strlen(&argv[0][2])); // Shift the flags down
        *next = argv;
        return true;
    }
    return false;
}

//
// Scan the first few dozen bytes of a file and return 1 if the contents all
// look like printable text characters, otherwise return 0.
//
static int is_text_file(const char *filename)
{
    int fd = open(filename, O_RDONLY);
    if (fd < 0) return 0;
    char buf[CHECK_FIRST_N_BYTES];
    ssize_t len = read(fd, buf, sizeof(buf)/sizeof(char));
    if (len < 0) return 0;
    (void)close(fd);

    for (ssize_t i = 0; i < len; i++) {
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
    for (match_t *m = NULL; (m = next_match(defs, f, m, pattern, options.skip, options.ignorecase)); ) {
        if (++matches > 1)
            printf(",\n");
        printf("{\"filename\":\"%s\",", f->filename);
        printf("\"tree\":{\"rule\":\"text\",\"start\":%d,\"end\":%ld,\"children\":[",
               0, f->end - f->contents);
        json_match(f->contents, m, options.verbose);
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
    for (match_t *m = NULL; (m = next_match(defs, f, m, pattern, options.skip, options.ignorecase)); ) {
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
    if (modifying_file && backup_file) {
        rewind(modifying_file);
        ftruncate(fileno(modifying_file), 0);
        fwrite(backup_file->contents, 1,
               (size_t)(backup_file->end - backup_file->contents),
               modifying_file);
        fclose(modifying_file);
        modifying_file = NULL;
    }
    if (backup_file) destroy_file(&backup_file);
}

//
// Signal handler to ensure cleanup happens.
//
static void sig_handler(int sig)
{
    cleanup();
    if (kill(0, sig)) _exit(EXIT_FAILURE);
}

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
            m->skip_replacement = true;
            goto check_children;
        }

        { // Print the original
            printer_t pr = {.file = f, .context_lines = options.context_lines,
                .use_color = true, .print_line_numbers = true};
            print_match(tty_out, &pr, m->child);
            // Print trailing context lines:
            print_match(tty_out, &pr, NULL);
        }
        if (options.context_lines > 1) fprintf(tty_out, "\n");
        { // Print the replacement
            printer_t pr = {.file = f, .context_lines = options.context_lines,
                .use_color = true, .print_line_numbers = true};
            print_match(tty_out, &pr, m);
            // Print trailing context lines:
            print_match(tty_out, &pr, NULL);
        }

      retry:
        fprintf(tty_out, "\033[1mReplace? (y)es (n)o (r)emaining (d)one\033[0m ");
        (void)fflush(tty_out);

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
    file_t *inmem_copy = NULL;
    // Ensure the file is resident in memory:
    if (f->mmapped) {
        inmem_copy = spoof_file(NULL, f->filename, f->contents, (ssize_t)(f->end - f->contents));
        f = inmem_copy;
    }

    printer_t pr = {
        .file = f,
        .context_lines = ALL_CONTEXT,
        .use_color = false,
        .print_line_numbers = false,
    };

    FILE *dest = NULL; // Lazy-open this on the first match
    int matches = 0;
    confirm_t confirm_file = options.confirm;
    for (match_t *m = NULL; (m = next_match(defs, f, m, pattern, options.skip, options.ignorecase)); ) {
        ++matches;
        printer_t err_pr = {.file = f, .context_lines = true, .use_color = true, .print_line_numbers = true};
        if (print_errors(&err_pr, m) > 0)
            exit(EXIT_FAILURE);
        // Lazy-open file for writing upon first match:
        if (dest == NULL) {
            dest = fopen(f->filename, "w");
            if (!dest)
                err(EXIT_FAILURE, "Failed to open %s for modification", f->filename);
            backup_file = f;
            modifying_file = dest;
            if (options.confirm == CONFIRM_ASK && f->filename)
                fprint_filename(tty_out, f->filename);
        }
        confirm_replacements(f, m, &confirm_file);
        print_match(dest, &pr, m);
    }

    if (dest) {
        // Print trailing context lines:
        print_match(dest, &pr, NULL);
        if (options.confirm == CONFIRM_ALL)
            printf("%s\n", f->filename);
        (void)fclose(dest);
        if (modifying_file == dest) modifying_file = NULL;
        if (backup_file == f) backup_file = NULL;
    }

    if (inmem_copy != NULL) destroy_file(&inmem_copy);

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
        .context_lines = options.context_lines,
        .use_color = options.format == FORMAT_FANCY,
        .print_line_numbers = options.format == FORMAT_FANCY,
    };

    for (match_t *m = NULL; (m = next_match(defs, f, m, pattern, options.skip, options.ignorecase)); ) {
        printer_t err_pr = {.file = f, .context_lines = true, .use_color = true, .print_line_numbers = true};
        if (print_errors(&err_pr, m) > 0)
            exit(EXIT_FAILURE);

        if (++matches == 1) {
            if (printed_filenames++ > 0) printf("\n");
            fprint_filename(stdout, f->filename);
        }
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
__attribute__((nonnull(2,3)))
static int process_file(def_t *defs, const char *filename, pat_t *pattern)
{
    file_t *f = load_file(NULL, filename);
    if (f == NULL) {
        fprintf(stderr, "Could not open file: %s\n%s\n", filename, strerror(errno));
        return 0;
    }

    int matches = 0;
    if (options.mode == MODE_EXPLAIN) {
        matches += explain_matches(defs, f, pattern);
    } else if (options.mode == MODE_LISTFILES) {
        match_t *m = next_match(defs, f, NULL, pattern, options.skip, options.ignorecase);
        if (m) {
            recycle_if_unused(&m);
            printf("%s\n", f->filename);
            matches += 1;
        }
    } else if (options.mode == MODE_JSON) {
        matches += print_matches_as_json(defs, f, pattern);
    } else if (options.mode == MODE_INPLACE) {
        matches += inplace_modify_file(defs, f, pattern);
    } else {
        matches += print_matches(defs, f, pattern);
    }

#ifdef DEBUG_HEAP
    if (recycle_all_matches() != 0)
        errx(EXIT_FAILURE, "Memory leak: there should no longer be any matches in use at this point.");
#endif
    destroy_file(&f);
    (void)fflush(stdout);
    return matches;
}

//
// Recursively process all non-dotfile files in the given directory.
//
__attribute__((nonnull(2,3)))
static int process_dir(def_t *defs, const char *dirname, pat_t *pattern)
{
    int matches = 0;
    glob_t globbuf;
    char globpath[PATH_MAX+1] = {'\0'};
    if (snprintf(globpath, PATH_MAX, "%s/*", dirname) > (int)PATH_MAX)
        errx(EXIT_FAILURE, "Filename is too long: %s/*", dirname);
    int status = glob(globpath, 0, NULL, &globbuf);
    if (status == GLOB_ABORTED || status == GLOB_NOSPACE)
        err(EXIT_FAILURE, "Failed to get directory contents: %s", dirname);
    if (status != GLOB_NOMATCH) {
        struct stat statbuf;
        for (size_t i = 0; i < globbuf.gl_pathc; i++) {
            if (lstat(globbuf.gl_pathv[i], &statbuf) != 0) continue;
            if (S_ISLNK(statbuf.st_mode))
                continue; // Skip symbolic links
            else if (S_ISDIR(statbuf.st_mode))
                matches += process_dir(defs, globbuf.gl_pathv[i], pattern);
            else if (is_text_file(globbuf.gl_pathv[i]))
                matches += process_file(defs, globbuf.gl_pathv[i], pattern);
        }
    }
    globfree(&globbuf);
    return matches;
}

//
// Process git files using `git ls-files ...`
//
__attribute__((nonnull(2)))
static int process_git_files(def_t *defs, pat_t *pattern, int argc, char *argv[])
{
    int fds[2];
    if (pipe(fds) != 0)
        err(EXIT_FAILURE, "Failed to create pipe");
    pid_t child = fork();
    if (child == -1)
        err(EXIT_FAILURE, "Failed to fork");
    if (child == 0) {
        char **git_args = memcheck(calloc((size_t)(2+argc+1), sizeof(char*)));
        int g = 0;
        git_args[g++] = "git";
        git_args[g++] = "ls-files";
        while (*argv) git_args[g++] = *(argv++);
        if (dup2(fds[STDOUT_FILENO], STDOUT_FILENO) != STDOUT_FILENO)
            err(EXIT_FAILURE, "Failed to hook up pipe to stdout");
        if (close(fds[STDIN_FILENO]) != 0)
            err(EXIT_FAILURE, "Failed to close read end of pipe");
        (void)execvp("git", git_args);
        _exit(EXIT_FAILURE);
    }
    if (close(fds[STDOUT_FILENO]) != 0)
        err(EXIT_FAILURE, "Failed to close write end of pipe");
    FILE *fp = fdopen(fds[STDIN_FILENO], "r");
    if (fp == NULL)
        err(EXIT_FAILURE, "Could not open file descriptor");
    char *path = NULL;
    size_t size = 0;
    ssize_t len = 0;
    int found = 0;
    while ((len = getline(&path, &size, fp)) > 0) {
        if (path[len-1] == '\n') path[len-1] = '\0';
        found += process_file(defs, path, pattern);
    }
    if (path) xfree(&path);
    if (fclose(fp) != 0)
        err(EXIT_FAILURE, "Failed to close read end of pipe");
    int status;
    while (waitpid(child, &status, 0) != child) continue;
    if (!((WIFEXITED(status) == 1) && (WEXITSTATUS(status) == 0)))
        errx(EXIT_FAILURE, "`git --ls-files` failed. Do you have git installed?");
    return found;
}

#define FLAG(f) (flag = get_flag(argv, f, &argv))
#define BOOLFLAG(f) get_boolflag(argv, f, &argv)

int main(int argc, char *argv[])
{
    char *flag = NULL;

    def_t *defs = NULL;
    file_t *loaded_files = NULL;
    pat_t *pattern = NULL;

    // Load builtins:
    file_t *xdg_file = load_filef(&loaded_files, "/etc/xdg/"BP_NAME"/builtins.bp");
    if (xdg_file) defs = load_grammar(defs, xdg_file);
    file_t *local_file = load_filef(&loaded_files, "%s/.config/"BP_NAME"/builtins.bp", getenv("HOME"));
    if (local_file) defs = load_grammar(defs, local_file);

    ++argv; // skip program name
    while (argv[0]) {
        if (streq(argv[0], "--")) {
            ++argv;
            break;
        } else if (BOOLFLAG("-h") || BOOLFLAG("--help")) {
            printf("%s\n\n%s\n", description, usage);
            exit(EXIT_SUCCESS);
        } else if (BOOLFLAG("-v") || BOOLFLAG("--verbose")) {
            options.verbose = true;
        } else if (BOOLFLAG("-e") || BOOLFLAG("--explain")) {
            options.mode = MODE_EXPLAIN;
        } else if (BOOLFLAG("-j") || BOOLFLAG("--json")) {
            options.mode = MODE_JSON;
        } else if (BOOLFLAG("-I") || BOOLFLAG("--inplace")) {
            options.mode = MODE_INPLACE;
        } else if (BOOLFLAG("-C") || BOOLFLAG("--confirm")) {
            options.confirm = CONFIRM_ASK;
        } else if (BOOLFLAG("-G") || BOOLFLAG("--git")) {
            options.git_mode = true;
        } else if (BOOLFLAG("-i") || BOOLFLAG("--ignore-case")) {
            options.ignorecase = true;
        } else if (BOOLFLAG("-l") || BOOLFLAG("--list-files")) {
            options.mode = MODE_LISTFILES;
        } else if (FLAG("-r")     || FLAG("--replace")) {
            if (!pattern)
                errx(EXIT_FAILURE, "No pattern has been defined for replacement to operate on");
            // TODO: spoof file as sprintf("pattern => '%s'", flag)
            // except that would require handling edge cases like quotation marks etc.
            file_t *replace_file = spoof_file(&loaded_files, "<replace argument>", flag, -1);
            pattern = bp_replacement(replace_file, pattern, replace_file->contents);
            if (!pattern)
                errx(EXIT_FAILURE, "Replacement failed to compile: %s", flag);
        } else if (FLAG("-g")     || FLAG("--grammar")) {
            file_t *f = NULL;
            if (strlen(flag) > 3 && strncmp(&flag[strlen(flag)-3], ".bp", 3) == 0)
                f = load_file(&loaded_files, flag);
            if (f == NULL)
                f = load_filef(&loaded_files, "%s/.config/"BP_NAME"/%s.bp", getenv("HOME"), flag);
            if (f == NULL)
                f = load_filef(&loaded_files, "/etc/xdg/"BP_NAME"/%s.bp", flag);
            if (f == NULL)
                errx(EXIT_FAILURE, "Couldn't find grammar: %s", flag);
            defs = load_grammar(defs, f); // Keep in memory for debug output
        } else if (FLAG("-p")     || FLAG("--pattern")) {
            file_t *arg_file = spoof_file(&loaded_files, "<pattern argument>", flag, -1);
            for (const char *str = arg_file->contents; str < arg_file->end; ) {
                def_t *d = bp_definition(defs, arg_file, str);
                if (d) {
                    defs = d;
                    str = after_spaces(d->pat->end);
                } else {
                    pat_t *p = bp_pattern(arg_file, str);
                    if (!p)
                        file_err(arg_file, str, arg_file->end,
                                 "Failed to compile this part of the argument");
                    pattern = chain_together(arg_file, pattern, p);
                    str = after_spaces(p->end);
                }
            }
        } else if (FLAG("-s")     || FLAG("--skip")) {
            file_t *arg_file = spoof_file(&loaded_files, "<skip argument>", flag, -1);
            pat_t *s = bp_pattern(arg_file, arg_file->contents);
            if (!s) {
                fprint_line(stdout, arg_file, arg_file->contents, arg_file->end,
                            "Failed to compile the skip argument");
            } else if (after_spaces(s->end) < arg_file->end) {
                fprint_line(stdout, arg_file, s->end, arg_file->end,
                            "Failed to compile part of the skip argument");
            }
            options.skip = either_pat(arg_file, options.skip, s);
        } else if (FLAG("-c")     || FLAG("--context")) {
            if (streq(flag, "all")) {
                options.context_lines = ALL_CONTEXT;
            } else if (streq(flag, "none")) {
                options.context_lines = 0;
            } else {
                options.context_lines = (int)strtol(flag, &flag, 10);
                if (flag && flag[0])
                    errx(EXIT_FAILURE, "Unsupported flags after --context: %s", flag);
            }
        } else if (FLAG("-f")      || FLAG("--format")) {
            options.format = streq(flag, "fancy") ? FORMAT_FANCY : streq(flag, "plain") ? FORMAT_PLAIN : FORMAT_AUTO;
        } else if (argv[0][0] == '-' && argv[0][1] && argv[0][1] != '-') { // single-char flags
            errx(EXIT_FAILURE, "Unrecognized flag: -%c\n\n%s", argv[0][1], usage);
        } else if (argv[0][0] != '-') {
            if (pattern != NULL) break;
            file_t *arg_file = spoof_file(&loaded_files, "<pattern argument>", argv[0], -1);
            pat_t *p = bp_stringpattern(arg_file, arg_file->contents);
            if (!p)
                errx(EXIT_FAILURE, "Pattern failed to compile: %s", argv[0]);
            pattern = chain_together(arg_file, pattern, p);
            ++argv;
        } else {
            errx(EXIT_FAILURE, "Unrecognized flag: %s\n\n%s", argv[0], usage);
        }
    }

    if (pattern == NULL)
        errx(EXIT_FAILURE, "No pattern provided.\n\n%s", usage);

    if (options.confirm == CONFIRM_ASK && options.mode != MODE_INPLACE)
        errx(EXIT_FAILURE, "Confirm mode (-C flag) can only be used with inplace mode (-I flag)");

    for (argc = 0; argv[argc]; ++argc) ; // update argc

    if (options.context_lines == USE_DEFAULT_CONTEXT) options.context_lines = 1;
    if (options.context_lines < 0 && options.context_lines != ALL_CONTEXT) options.context_lines = 0;

    if (options.format == FORMAT_AUTO)
        options.format = isatty(STDOUT_FILENO) ? FORMAT_FANCY : FORMAT_PLAIN;

    // If any of these signals triggers, and there is a temporary file in use,
    // be sure to clean it up before exiting.
    int signals[] = {SIGTERM, SIGINT, SIGXCPU, SIGXFSZ, SIGVTALRM, SIGPROF, SIGSEGV, SIGTSTP};
    struct sigaction sa = {.sa_handler = &sig_handler, .sa_flags = (int)(SA_NODEFER | SA_RESETHAND)};
    for (size_t i = 0; i < sizeof(signals)/sizeof(signals[0]); i++)
        if (sigaction(signals[i], &sa, NULL) != 0)
            err(EXIT_FAILURE, "Failed to set signal handler");

    // Handle exit() calls gracefully:
    if (atexit(&cleanup) != 0)
        err(EXIT_FAILURE, "Failed to set cleanup handler at exit");

    // User input/output is handled through /dev/tty so that normal unix pipes
    // can work properly while simultaneously asking for user input.
    if (options.confirm == CONFIRM_ASK) {
        tty_in = fopen("/dev/tty", "r");
        tty_out = fopen("/dev/tty", "w");
    }

    // To ensure recursion (and left recursion in particular) works properly,
    // we need to define a rule called "pattern" with the value of whatever
    // pattern the args specified, and use `pattern` as the thing being matched.
    defs = with_def(defs, strlen("pattern"), "pattern", pattern);
    file_t *patref_file = spoof_file(&loaded_files, "<pattern ref>", "pattern", -1);
    pattern = bp_pattern(patref_file, patref_file->contents);

    int found = 0;
    if (options.mode == MODE_JSON) printf("[");
    if (options.git_mode) { // Get the list of files from `git --ls-files ...`
        found = process_git_files(defs, pattern, argc, argv);
    } else if (argv[0]) {
        // Files pass in as command line args:
        struct stat statbuf;
        for ( ; argv[0]; argv++) {
            if (stat(argv[0], &statbuf) == 0 && S_ISDIR(statbuf.st_mode)) // Symlinks are okay if manually specified
                found += process_dir(defs, argv[0], pattern);
            else
                found += process_file(defs, argv[0], pattern);
        }
    } else if (isatty(STDIN_FILENO)) {
        // No files, no piped in input, so use files in current dir, recursively
        found += process_dir(defs, ".", pattern);
    } else {
        // Piped in input:
        found += process_file(defs, "", pattern);
    }
    if (options.mode == MODE_JSON) printf("]\n");

    if (tty_out) { (void)fclose(tty_out); tty_out = NULL; }
    if (tty_in) { (void)fclose(tty_in); tty_in = NULL; }

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

    exit(found > 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
