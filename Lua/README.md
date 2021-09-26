# Lua Bindings

This directory contains Lua bindings for bp. The bindings are intended to be
roughly equivalent in usefulness to LPEG, but with a smaller codebase (roughly
3/4 the size, as of this writing).

## API

The Lua `bp` bindings provide the following methods:

```lua
bp.match(pattern, text, [start_index]) --> match / nil
bp.replace(pattern, replacement, text, [start_index]) --> text_with_replacements, num_replacements
bp.compile(pattern) --> pattern_object
for m in bp.matches(pattern, text, [start_index]) do ... end

pattern_object:match(text, [start_index]) --> match / nil
pattern_object:replace(replacement, text, [start_index]) --> text_with_replacements, num_replacements
for m in pattern_object:matches(text, [start_index]) do ... end
```

Match objects returned by `bp.match()` are tables whose `__tostring` will
return the text of the match. Additionally, match objects store the text of the
match at index `0`, the match's starting index in the source string as
`.start`, the first index after the match as `.after`, and any captures stored
as match objects with a key corresponding to the capture's identifier (e.g.
`@"a" @foo="bc"` will be encoded as `{[0]="abc", [1]="a", foo={[0]="bc"}}`. If
multiple captures within a match share the same identifier, it is unspecified
which captured match will be stored at that key, so it's best to be
unambiguous.

Pattern objects returned by `bp.compile()` are pre-compiled patterns that are
slightly faster to reuse than just calling `bp.match()` repeatedly. They have
`:match()`, `:replace()`, and `:matches()` methods as described above.

All methods will raise an error with a descriptive message if the given pattern
has a syntax error.

## Example Usage

```lua
local bp = require("bp")
local m = bp.match('"n" @Es=+`e "dle"', "like finding a needle in a haystack")
--> {[0]="needle", Es={[0]="ee", start=17, after=19}, start=16, after=22}
--> tostring(m) == "needle", tostring(m.Es) == "ee"
local replaced, nreplacements = bp.match('"n" +`e "dle"', "cat", "like finding a needle in a haystack")
--> "like finding a cat in a haystack", 1

for word in bp.matches("+`A-Z,a-z", "one, two three... four!") do
    print(word) --> prints "one" "two" "three" "four"
end

local pat = bp.compile("word parens")
for _,s in ipairs(my_strings) do
    for fncall in pat:matches(s) do
        print(fncall)
    end
end
```
