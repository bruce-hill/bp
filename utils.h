/*
 * utils.h - Some helper code for debugging and error logging.
 */

// TODO: better error reporting
#define check(cond, ...) do { if (!(cond)) { fprintf(stderr, __VA_ARGS__); fwrite("\n", 1, 1, stderr); _exit(1); } } while(0)
#define debug(...) do { if (verbose) fprintf(stderr, __VA_ARGS__); } while(0)
