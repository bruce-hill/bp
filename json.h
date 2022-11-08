//
// json.h - Header file for JSON output.
//
#pragma once

#include <stdbool.h>

#include "match.h"

__attribute__((nonnull))
void json_match(const char *text, match_t *m, bool verbose);

// vim: ts=4 sw=0 et cino=L2,l1,(0,W4,m1,\:0
