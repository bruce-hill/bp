/*
 * json.h - Header file for JSON output.
 */
#ifndef JSON__H
#define JSON__H

__attribute__((nonnull))
void json_match(FILE *f, const char *text, match_t *m, int verbose);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1
