//
// json.h - Header file for JSON output.
//
#ifndef JSON__H
#define JSON__H

#include <stdbool.h>

#include "match.h"

__attribute__((nonnull))
void json_match(const char *text, match_t *m, bool verbose);

#endif
// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
