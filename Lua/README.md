# Lua Bindings

This directory contains Lua bindings for bp. The bindings are intended to be
roughly equivalent in usefulness to LPEG, but with a smaller codebase (roughly
3/4 the size, as of this writing).

## API

The Lua `bp` bindings provide the following methods:

```lua
bp.match(text, pattern, [start_index]) --> match, match_start, match_length / nil
bp.replace(text, pattern, replacement, [start_index]) --> text_with_replacements, num_replacements
```

Match objects returned by `bp.match()` are tables whose `__tostring` will
return the text of the match. Additionally, match objects store the text of the
match at index `0`, and any captures stored as match objects with a key
corresponding to the capture's identifier (e.g. `@"a" @foo="bc"` will be
encoded as `{[0]="abc", [1]="a", foo={[0]="bc"}}`. If multiple captures within
a match share the same identifier, it is unspecified which captured match will
be stored at that key, so it's best to be unambiguous.

All methods will raise an error with a descriptive message if the given pattern
has a syntax error.

## Example Usage

```lua
local bp = require("bp")
local m, i, len = bp.match("like finding a needle in a haystack", '"n" @Es=+`e "dle"')
--> {[0]="needle", Es={[0]="ee"}}, 16, 6
--> tostring(m) == "needle", tostring(m.Es) == "ee"
local replaced, nreplacements = bp.match("like finding a needle in a haystack", '"n" +`e "dle"', "cat")
--> "like finding a cat in a haystack", 1
```
