//
// bp.c - Source code for the bp parser
//
// See `man ./bp.1` for more details
//

#include <ctype.h>
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
#include "explain.h"
#include "files.h"
#include "json.h"
#include "match.h"
#include "pattern.h"
#include "utils.h"

#ifndef BP_NAME
#define BP_NAME "bp"
#endif

static const char *description = (
    BP_NAME" - a Parsing Expression Grammar command line tool");
static const char *usage = (
    "Usage:\n"
    "  "BP_NAME" [flags] <pattern> [<files>...]\n\n"
    "Flags:\n"
    " -h --help                        print the usage and quit\n"
    " -v --verbose                     print verbose debugging info\n"
    " -e --explain                     explain the matches\n"
    " -j --json                        print matches as a list of JSON objects\n"
    " -i --ignore-case                 preform matching case-insensitively\n"
    " -I --inplace                     modify a file in-place\n"
    " -l --list-files                  list filenames only\n"
    " -p --pattern <pat>               provide a pattern (equivalent to bp '\\(<pat>)')\n"
    " -w --word <string-pat>           find words matching the given string pattern\n"
    " -r --replace <replacement>       replace the input pattern with the given replacement\n"
    " -s --skip <skip-pattern>         skip over the given pattern when looking for matches\n"
    " -B --context-before <n>          set number of lines of context to print before the match\n"
    " -B --context-after <n>           set number of lines of context to print after the match\n"
    " -C --context <context>           set number of lines of context to print before and after the match\n"
    " -f --format fancy|plain|bare|file:line    set the output format\n"
    " -g --grammar <grammar-file>      use the specified file as a grammar");

// Used as a heuristic to check if a file is binary or text:
#define CHECK_FIRST_N_BYTES 256

#define USE_DEFAULT_CONTEXT -3
#define ALL_CONTEXT -2
#define NO_CONTEXT -1

// Flag-configurable options:
static struct {
    int context_before, context_after;
    bool ignorecase, verbose, git_mode, print_filenames;
    enum { MODE_NORMAL, MODE_LISTFILES, MODE_INPLACE, MODE_JSON, MODE_EXPLAIN } mode;
    enum { FORMAT_AUTO, FORMAT_FANCY, FORMAT_PLAIN, FORMAT_BARE, FORMAT_FILE_LINE } format;
    pat_t *skip;
} options = {
    .context_before = USE_DEFAULT_CONTEXT,
    .context_after = USE_DEFAULT_CONTEXT,
    .ignorecase = false,
    .print_filenames = true,
    .verbose = false,
    .mode = MODE_NORMAL,
    .format = FORMAT_AUTO,
    .skip = NULL,
};

const char *LINE_FORMATS[] = {
    [FORMAT_FANCY] = "\033[0;2m#\033(0\x78\033(B",
    [FORMAT_PLAIN] = "#|",
    [FORMAT_BARE] = "",
    [FORMAT_FILE_LINE] = "@:#0:",
};

// If a file is partly through being modified when the program exits, restore it from backup.
static FILE *modifying_file = NULL;
static file_t *backup_file;

//
// Helper function to reduce code duplication
//
static inline void fprint_filename(FILE *out, const char *filename)
{
    if (!filename[0]) return;
    if (options.format == FORMAT_FANCY) fprintf(out, "\033[0;1;4;33m%s\033[m\n", filename);
    else fprintf(out, "%s:\n", filename);
}

//
// If there was a parse error while building a pattern, print an error message and exit.
//
static inline pat_t *assert_pat(const char *start, maybe_pat_t maybe_pat)
{
    if (!maybe_pat.success) {
        const char *err_start = maybe_pat.value.error.start,
              *err_end = maybe_pat.value.error.end,
              *err_msg = maybe_pat.value.error.msg;

        const char *sol = memrchr(start, '\n', (size_t)(err_start - 1 - start));
        if (sol) start = sol+1;
        const char *eol = memchr(err_start, '\n', (size_t)(err_end - err_start));
        if (eol) err_end = eol;
        const char *end = eol ? eol : strchr(err_end, '\n');

        fprintf(stderr, "\033[31;1m%s\033[0m\n", err_msg);
        fprintf(stderr, "%.*s\033[41;30m%.*s\033[m%.*s\n",
                (int)(err_start - start), start,
                (int)(err_end - err_start), err_start,
                (int)(end - err_end), err_end);
        fprintf(stderr, "\033[34;1m");
        const char *p = start;
        for (; p < err_start; ++p) (void)fputc(*p == '\t' ? '\t' : ' ', stderr);
        if (err_start == err_end) ++err_end;
        for (; p < err_end; ++p)
            if (*p == '\t')
                // Some janky hacks: 8 ^'s, backtrack 8 spaces, move forward a tab stop, clear any ^'s that overshot
                fprintf(stderr, "^^^^^^^^\033[8D\033[I\033[K");
            else
                (void)fputc('^', stderr);
        fprintf(stderr, "\033[m\n");
        exit(EXIT_FAILURE);
    }
    return maybe_pat.value.pat;
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
    (void)close(fd);
    if (len < 0) return 0;
    for (ssize_t i = 0; i < len; i++)
        if (isascii(buf[i]) && !(isprint(buf[i]) || isspace(buf[i])))
            return 0;
    return 1;
}

//
// Print matches in JSON format.
//
static int print_matches_as_json(def_t *defs, file_t *f, pat_t *pattern)
{
    int nmatches = 0;
    for (match_t *m = NULL; next_match(&m, defs, f->start, f->end, pattern, options.skip, options.ignorecase); ) {
        if (++nmatches > 1)
            printf(",\n");
        printf("{\"filename\":\"%s\",\"match\":", f->filename);
        json_match(f->start, m, options.verbose);
        printf("}");
    }
    return nmatches;
}

//
// Print matches in a visual explanation style
//
static int explain_matches(def_t *defs, file_t *f, pat_t *pattern)
{
    int nmatches = 0;
    for (match_t *m = NULL; next_match(&m, defs, f->start, f->end, pattern, options.skip, options.ignorecase); ) {
        if (++nmatches == 1) {
            if (options.print_filenames)
                fprint_filename(stdout, f->filename);
        } else
            printf("\n\n");
        explain_match(m);
    }
    return nmatches;
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
        (void)fwrite(backup_file->start, 1,
                     (size_t)(backup_file->end - backup_file->start),
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

void fprint_linenum(FILE *out, file_t *f, int linenum, const char *normal_color)
{
    switch (options.format) {
    case FORMAT_FANCY: case FORMAT_PLAIN: {
        int space = 0;
        for (int i = (int)f->nlines; i > 0; i /= 10) ++space;
        if (options.format == FORMAT_FANCY)
            fprintf(out, "\033[0;2m%*d\033(0\x78\033(B%s", space, linenum, normal_color ? normal_color : "");
        else fprintf(out, "%*d|", space, linenum);
        break;
    }
    case FORMAT_FILE_LINE: {
        fprintf(out, "%s:%d:", f->filename, linenum);
        break;
    }
    default: break;
    }
}

static file_t *printing_file = NULL;
static int last_line_num = -1;
static void _fprint_between(FILE *out, const char *start, const char *end, const char *normal_color)
{
    if (!end) end = printing_file->end;
    while (start < end) {
        if (!start) start = printing_file->start;
        if (start == printing_file->start || start[-1] == '\n') {
            int linenum = (int)get_line_number(printing_file, start);
            if (last_line_num != linenum) {
                fprint_linenum(out, printing_file, linenum, normal_color);
                last_line_num = linenum;
            }
        }
        const char *line_end = memchr(start, '\n', (size_t)(end - start));
        if (line_end && line_end != end) {
            fwrite(start, sizeof(char), (size_t)(line_end - start + 1), out);
            start = line_end + 1;
        } else {
            fwrite(start, sizeof(char), (size_t)(end - start), out);
            break;
        }
    }
}

static void fprint_context_between(FILE *out, const char *prev, const char *next)
{
    if (!prev && !next) return;
    if (options.context_before == ALL_CONTEXT || options.context_after == ALL_CONTEXT) {
        _fprint_between(out, prev, next, "\033[m");
        return;
    }
    const char *after_prev = prev, *before_next = next;
    if (prev && options.context_after >= 0) {
        size_t after_prev_line = get_line_number(printing_file, prev) + (size_t)options.context_after + 1;
        after_prev = after_prev_line > printing_file->nlines ? printing_file->end : get_line(printing_file, after_prev_line > printing_file->nlines ? printing_file->nlines : after_prev_line);
    }
    if (next && options.context_before >= 0) {
        size_t before_next_line = get_line_number(printing_file, next);
        before_next_line = options.context_before >= (int)before_next_line ? 1 : before_next_line - (size_t)options.context_before;
        before_next = get_line(printing_file, before_next_line);
    }
    if (!prev) {
        _fprint_between(out, before_next, next, "\033[m");
    } else if (!next) {
        _fprint_between(out, prev, after_prev, "\033[m");
    } else if (after_prev >= before_next) {
        _fprint_between(out, prev, next, "\033[m");
    } else {
        _fprint_between(out, prev, after_prev, "\033[m");
        _fprint_between(out, before_next, next, "\033[m");
    }
}

static void on_nl(FILE *out)
{
    switch (options.format) {
    case FORMAT_FANCY: case FORMAT_PLAIN:
        for (int i = (int)printing_file->nlines; i > 0; i /= 10) fputc('.', out);
        fprintf(out, "%s", options.format == FORMAT_FANCY ? "\033[0;2m\033(0\x78\033(B\033[m" : "|");
        break;
    default: break;
    }
}

//
// Print all the matches in a file.
//
static int print_matches(FILE *out, def_t *defs, file_t *f, pat_t *pattern)
{
    static int printed_filenames = 0;
    int matches = 0;
    const char *prev = NULL;

    printing_file = f;
    last_line_num = -1;

    print_options_t print_opts = {.fprint_between = _fprint_between, .on_nl = on_nl};
    if (options.format == FORMAT_FANCY) {
        print_opts.match_color = "\033[0;31;1m";
        print_opts.replace_color = "\033[0;34;1m";
        print_opts.normal_color = "\033[m";
    }
    for (match_t *m = NULL; next_match(&m, defs, f->start, f->end, pattern, options.skip, options.ignorecase); ) {
        if (++matches == 1 && options.print_filenames) {
            if (printed_filenames++ > 0) printf("\n");
            fprint_filename(out, f->filename);
        }
        fprint_context_between(out, prev, m->start);
        if (print_opts.normal_color) fprintf(out, "%s", print_opts.normal_color);
        fprint_match(out, f->start, m, &print_opts);
        if (print_opts.normal_color) fprintf(out, "%s", print_opts.normal_color);
        prev = m->end;
    }
    if (matches > 0)
        fprint_context_between(out, prev, NULL);

    printing_file = NULL;
    last_line_num = -1;
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
        match_t *m = NULL;
        if (next_match(&m, defs, f->start, f->end, pattern, options.skip, options.ignorecase)) {
            printf("%s\n", f->filename);
            matches += 1;
        }
        stop_matching(&m);
    } else if (options.mode == MODE_JSON) {
        matches += print_matches_as_json(defs, f, pattern);
    } else if (options.mode == MODE_INPLACE) {
        match_t *m = NULL;
        bool found = next_match(&m, defs, f->start, f->end, pattern, options.skip, options.ignorecase);
        stop_matching(&m);
        if (!found) return 0;

        // Ensure the file is resident in memory:
        if (f->mmapped) {
            file_t *copy = spoof_file(NULL, f->filename, f->start, (ssize_t)(f->end - f->start));
            destroy_file(&f);
            f = copy;
        }
        FILE *out = fopen(filename, "w");
        // Set these temporary values in case the program crashes while in the
        // middle of inplace modifying a file. If that happens, these variables
        // are used to restore the original file contents.
        modifying_file = out; backup_file = f;
        {
            matches += print_matches(out, defs, f, pattern);
        }
        modifying_file = NULL; backup_file = NULL;
        fclose(out);
    } else {
        matches += print_matches(stdout, defs, f, pattern);
    }
    fflush(stdout);

    if (recycle_all_matches() != 0)
        fprintf(stderr, "\033[33;1mMemory leak: there should no longer be any matches in use at this point.\033[m\n");
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
        errx(EXIT_FAILURE, "Failed to get directory contents: %s", dirname);
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
    require(pipe(fds), "Failed to create pipe");
    pid_t child = require(fork(), "Failed to fork");
    if (child == 0) {
        const char **git_args = new(char*[3+argc+1]);
        int g = 0;
        git_args[g++] = "git";
        git_args[g++] = "ls-files";
        git_args[g++] = "-z";
        while (*argv) git_args[g++] = *(argv++);
        require(dup2(fds[STDOUT_FILENO], STDOUT_FILENO), "Failed to hook up pipe to stdout");
        require(close(fds[STDIN_FILENO]), "Failed to close read end of pipe");
        (void)execvp("git", (char**)git_args);
        _exit(EXIT_FAILURE);
    }
    require(close(fds[STDOUT_FILENO]), "Failed to close write end of pipe");
    FILE *fp = require(fdopen(fds[STDIN_FILENO], "r"), "Could not open pipe file descriptor");
    char *path = NULL;
    size_t path_size = 0;
    int found = 0;
    while (getdelim(&path, &path_size, '\0', fp) > 0)
        found += process_file(defs, path, pattern);
    if (path) delete(&path);
    require(fclose(fp), "Failed to close read end of pipe");
    int status;
    while (waitpid(child, &status, 0) != child) continue;
    if (!((WIFEXITED(status) == 1) && (WEXITSTATUS(status) == 0)))
        errx(EXIT_FAILURE, "`git ls-files -z` failed.");
    return found;
}

//
// Load the given grammar (semicolon-separated definitions)
// and return the first rule defined.
//
static def_t *load_grammar(def_t *defs, file_t *f)
{
    pat_t *pat = assert_pat(f->start, bp_pattern(f->start, f->end));
    for (pat_t *p = pat; p && p->type == BP_DEFINITION; p = p->args.def.pat)
        defs = with_def(defs, p->args.def.namelen, p->args.def.name, p->args.def.def);
    return defs;
}

//
// Convert a context string to an integer
//
static int context_from_flag(const char *flag)
{
    if (streq(flag, "all")) return ALL_CONTEXT;
    if (streq(flag, "none")) return NO_CONTEXT;
    return (int)strtol(flag, NULL, 10);
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
    file_t *builtins_file = load_file(&loaded_files, "/etc/"BP_NAME"/builtins.bp");
    if (builtins_file) defs = load_grammar(defs, builtins_file);
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
            options.print_filenames = false;
            options.format = FORMAT_BARE;
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
            pattern = assert_pat(flag, bp_replacement(pattern, flag, flag+strlen(flag)));
            if (options.context_before == USE_DEFAULT_CONTEXT) options.context_before = ALL_CONTEXT;
            if (options.context_after == USE_DEFAULT_CONTEXT) options.context_after = ALL_CONTEXT;
        } else if (FLAG("-g")     || FLAG("--grammar")) {
            file_t *f = NULL;
            if (strlen(flag) > 3 && strncmp(&flag[strlen(flag)-3], ".bp", 3) == 0)
                f = load_file(&loaded_files, flag);
            if (f == NULL)
                f = load_filef(&loaded_files, "%s/.config/"BP_NAME"/%s.bp", getenv("HOME"), flag);
            if (f == NULL)
                f = load_filef(&loaded_files, "/etc/"BP_NAME"/%s.bp", flag);
            if (f == NULL)
                errx(EXIT_FAILURE, "Couldn't find grammar: %s", flag);
            defs = load_grammar(defs, f); // Keep in memory for debug output
        } else if (FLAG("-p")     || FLAG("--pattern")) {
            pat_t *p = assert_pat(flag, bp_pattern(flag, flag+strlen(flag)));
            pattern = chain_together(pattern, p);
        } else if (FLAG("-w")     || FLAG("--word")) {
            require(asprintf(&flag, "\\|%s\\|", flag), "Could not allocate memory");
            file_t *arg_file = spoof_file(&loaded_files, "<word pattern>", flag, -1);
            delete(&flag);
            pat_t *p = assert_pat(arg_file->start, bp_stringpattern(arg_file->start, arg_file->end));
            pattern = chain_together(pattern, p);
        } else if (FLAG("-s")     || FLAG("--skip")) {
            pat_t *s = assert_pat(flag, bp_pattern(flag, flag+strlen(flag)));
            options.skip = either_pat(options.skip, s);
        } else if (FLAG("-C")     || FLAG("--context")) {
            options.context_before = options.context_after = context_from_flag(flag);
        } else if (FLAG("-B")     || FLAG("--before-context")) {
            options.context_before = context_from_flag(flag);
        } else if (FLAG("-A")     || FLAG("--after-context")) {
            options.context_after = context_from_flag(flag);
        } else if (FLAG("-f")      || FLAG("--format")) {
            if (streq(flag, "fancy")) options.format = FORMAT_FANCY;
            else if (streq(flag, "plain")) options.format = FORMAT_PLAIN;
            else if (streq(flag, "bare")) options.format = FORMAT_BARE;
            else if (streq(flag, "file:line")) {
                options.format = FORMAT_FILE_LINE;
                options.print_filenames = 0;
            } else if (!streq(flag, "auto"))
                errx(EXIT_FAILURE, "Unknown --format option: %s", flag);
        } else if (argv[0][0] == '-' && argv[0][1] && argv[0][1] != '-') { // single-char flags
            errx(EXIT_FAILURE, "Unrecognized flag: -%c\n\n%s", argv[0][1], usage);
        } else if (argv[0][0] != '-') {
            if (pattern != NULL) break;
            pat_t *p = assert_pat(argv[0], bp_stringpattern(argv[0], argv[0]+strlen(argv[0])));
            pattern = chain_together(pattern, p);
            ++argv;
        } else {
            errx(EXIT_FAILURE, "Unrecognized flag: %s\n\n%s", argv[0], usage);
        }
    }

    if (pattern == NULL)
        errx(EXIT_FAILURE, "No pattern provided.\n\n%s", usage);

    for (argc = 0; argv[argc]; ++argc) ; // update argc

    if (options.context_before == USE_DEFAULT_CONTEXT) options.context_before = 0;
    if (options.context_after == USE_DEFAULT_CONTEXT) options.context_after = 0;

    if (options.format == FORMAT_AUTO)
        options.format = isatty(STDOUT_FILENO) ? FORMAT_FANCY : FORMAT_BARE;

    // If any of these signals triggers, and there is a temporary file in use,
    // be sure to clean it up before exiting.
    int signals[] = {SIGTERM, SIGINT, SIGXCPU, SIGXFSZ, SIGVTALRM, SIGPROF, SIGSEGV, SIGTSTP};
    struct sigaction sa = {.sa_handler = &sig_handler, .sa_flags = (int)(SA_NODEFER | SA_RESETHAND)};
    for (size_t i = 0; i < sizeof(signals)/sizeof(signals[0]); i++)
        require(sigaction(signals[i], &sa, NULL), "Failed to set signal handler");

    // Handle exit() calls gracefully:
    require(atexit(&cleanup), "Failed to set cleanup handler at exit");

    int found = 0;
    if (options.mode == MODE_JSON) printf("[");
    if (options.git_mode) { // Get the list of files from `git --ls-files ...`
        found = process_git_files(defs, pattern, argc, argv);
    } else if (argv[0]) {
        // Files pass in as command line args:
        struct stat statbuf;
        if (!argv[1] && !(stat(argv[0], &statbuf) == 0 && S_ISDIR(statbuf.st_mode))) // Don't print filename for single-file matching
            options.print_filenames = false;
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
        options.print_filenames = false; // Don't print filename on stdin
        found += process_file(defs, "", pattern);
    }
    if (options.mode == MODE_JSON) printf("]\n");

    // This code frees up all residual heap-allocated memory. Since the program
    // is about to exit, this step is unnecessary. However, it is useful for
    // tracking down memory leaks.
    free_all_matches();
    defs = free_defs(defs, NULL);
    free_pat(NULL);
    while (loaded_files) {
        file_t *next = loaded_files->next;
        destroy_file(&loaded_files);
        loaded_files = next;
    }

    exit(found > 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
