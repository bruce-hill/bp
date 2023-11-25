% BP(1)
% Bruce Hill (*bruce@bruce-hill.com*)
% May 17 2021

# NAME

bp - Bruce\'s Parsing Expression Grammar tool

# SYNOPSIS

`bp` \[*options...*\] *pattern* \[\[`--`\] *files...*\]

# DESCRIPTION

`bp` is a tool that matches parsing expression grammars using a custom
syntax.

# OPTIONS

*pattern*
: The text to search for. The main argument for `bp` is a string literals which
  may contain BP syntax patterns. See the **STRING PATTERNS** section below.

`-w`, `--word` *word*
: Surround a string pattern with word boundaries (equivalent to `bp '{|}word{|}'`)

`-e`, `--explain`
: Print a visual explanation of the matches.

`-j`, `--json`
: Print a JSON list of the matches. (Pairs with `--verbose` for more detail)

`-l`, `--list-files`
: Print only the names of files containing matches instead of the matches
themselves.

`-i`, `--ignore-case`
: Perform pattern matching case-insensitively.

`-I`, `--inplace`
: Perform filtering or replacement in-place (i.e. overwrite files with new
content).

`-r`, `--replace` *replacement*
: Replace all occurrences of the main pattern with the given string.

`-s`, `--skip` *pattern*
: While looking for matches, skip over *pattern* occurrences. This can be
useful for behavior like `bp -s string` (avoiding matches inside string
literals).

`-g`, `--grammar` *grammar-file*
: Load the grammar from the given file. See the `GRAMMAR FILES` section
for more info.

`-G`, `--git`
: Use `git` to get a list of files. Remaining file arguments (if any) are
passed to `git --ls-files` instead of treated as literal files.

`-B`, `--context-before` *N*
: The number of lines of context to print before each match (default: 0). See
`--context` below for details on `none` or `all`.

`-A`, `--context-after` *N*
: The number of lines of context to print after each match (default: 0). See
`--context` below for details on `none` or `all`.


`-C`, `--context` *N*
: The number of lines to print before and after each match (default: 0). If *N*
is `none`, print only the exact text of the matches. If *N* is **"all"**, print
all text before and after each match.

`-f`, `--format` *fancy*\|*plain*\|*bare*\|*file:line*\|*auto*
: Set the output format. *fancy* includes colors and line numbers, *plain*
prints line numbers with no coloring, *bare* prints only the match text,
*file:line* prints the filename and line number for each match (grep-style),
and *auto* (the default) uses *fancy* formatting when the output is a TTY and
*bare* formatting otherwise.

`-h`, `--help`
: Print the usage and exit.

*files...*
: The input files to search. If no input files are provided and data was piped
in, that data will be used instead. If neither are provided, `bp` will search
through all files in the current directory and its subdirectories
(recursively).


# STRING PATTERNS

One of the most common use cases for pattern matching tools is matching plain,
literal strings, or strings that are primarily plain strings, with one or two
patterns. `bp` is designed around this fact. The default mode for bp patterns
is "string pattern mode". In string pattern mode, all characters are
interpreted literally except for curly braces `{}`, which mark a region of BP
syntax patterns (see the **PATTERNS** section below). In other words, when
passing a search query to `bp`, you do not need to escape periods, quotation
marks, backslashes, or any other character, as long as it fits inside a shell
string literal. In order to match a literal `{`, you can either search for the
character literal: ``` {`{} ```, the string literal: `{"{"}`, or a pair of 
matching curly braces using the `braces` rule: `{braces}`.


# PATTERNS

`bp` patterns are based off of a combination of Parsing Expression Grammars and
regular expression syntax. The syntax is designed to map closely to verbal
descriptions of the patterns, and prefix operators are preferred over suffix
operators (as is common in regex syntax). Patterns are whitespace-agnostic, so
they work the same regardless of whether whitespace is present or not, except
for string literals (`'...'` and `"..."`), character literals (`` ` ``), and
escape sequences (`\`). Whitespace between patterns or parts of a pattern
should be used for clarity, but it will not affect the meaning of the pattern.

*pat1 pat2*
: A sequence: *pat1* followed by *pat2*

*pat1* `/` *pat2*
: A choice: *pat1*, or if it doesn\'t match, then *pat2*

`.`
: The period pattern matches single character (excluding newline)

`^`
: Start of a line

`^^`
: Start of the text

`$`
: End of a line (does not include newline character)

`$$`
: End of the text

`_`
: Zero or more whitespace characters, including spaces and tabs, but not
newlines.

`__`
: Zero or more whitespace characters, including spaces, tabs, newlines, and
comments. Comments are undefined by default, but may be defined by a separate
grammar file. See the **GRAMMAR FILES** section for more info.

`"foo"`, `'foo'`
: The literal string **"foo"**. Single and double quotes are treated the same.
Escape sequences are not allowed.

`` ` ``*c*
: The literal character *c* (e.g. `` `@ `` matches the "@" character)

`` ` ``*c1*`-`*c2*
: The character range *c1* to *c2* (e.g. `` `a-z ``). Multiple ranges
can be combined with a comma (e.g. `` `a-z,A-Z ``).

`` ` ``*c1*`,`*c2*
: Any one of the given character or character ranges *c1* or *c2* (e.g. `` `a,e,i,o,u,0-9 ``)

`\`*esc*
: An escape sequence (e.g. `\n`, `\x1F`, `\033`, etc.)

`\`*esc1*`-`*esc2*
: An escape sequence range from *esc1* to *esc2* (e.g. `\x00-x1F`)

`\`*esc1*`,`*esc2*
: Any one of the given escape sequences or ranges *esc1* or *esc2* (e.g. `\r,n,x01-x04`)

`\N`
: A special escape that matches a "nodent": one or more newlines followed by
the same indentation that occurs on the current line.

`\C`
: A special escape that always matches the empty string and replaces it with
the indentation of the line on which it matched. For example, this pattern
would match Bash-style heredocs that start with "<<-FOO" and end with a line
containing only the starting indentation and the string "FOO":
`"<<-" @end=(\C id) ..%\n (^end$)`

`\i`
: An identifier character (e.g. alphanumeric characters or underscores).

`\I`
: An identifier character, not including numbers (e.g. alphabetic characters or underscores).

`|`
: A word boundary (i.e. the edge of a word).

`\b`
: Alias for `|` (word boundary)

`(` *pat* `)`
: Parentheses can be used to delineate patterns, as in most languages.

`!` *pat*
: Not *pat* (don't match if *pat* matches here)

`[` *pat* `]`
: Maybe *pat* (match zero or one occurrences of *pat*)

*N* *pat*
: Exactly *N* repetitions of *pat* (e.g. `5 "x"` matches **"xxxxx"**)

*N* `-` *M* *pat*
: Between *N* and *M* repetitions of *pat* (e.g. `2-3 "x"` matches **"xx"** or
  **"xxx"**)

*N*`+` *pat*
: At least *N* or more repetitions of *pat* (e.g. `2+ "x"` matches **"xx"**,
  **"xxx"**, **"xxxx"**, etc.)

`*` *pat*
: Any *pat*s (zero or more, e.g. `* "x"` matches **""**, **"x"**, **"xx"**,
  etc.)

`+` *pat*
: Some *pat*s (one or more, e.g. `+ "x"` matches **"x"**, **"xx"**, **"xxx"**,
  etc.)

*repeating-pat* `%` *sep*
: *repeating-pat* (see the examples above) separated by *sep* (e.g. `*word %
  ","` matches zero or more comma-separated words)

`..` *pat*
: Any text (except newlines) up to and including *pat*. This is a non-greedy
  match and does not span newlines.

`.. %` *skip* *pat*
: Any text (except newlines) up to and including *pat*, skipping over instances
  of *skip* (e.g. `'"' ..%('\' .) '"'` opening quote, up to closing quote,
  skipping over backslash followed by a single character). A useful application
  of the `%` operator is to skip over newlines to perform multi-line matches,
  e.g. `pat1 ..%\n pat2`

`.. =` *only* *pat*
: Any number of repetitions of the pattern *only* up to and including *pat*
  (e.g. `"f" ..=abc "k"` matches the letter "f" followed by some alphabetic
  characters and then a "k", which would match "fork", but not "free kit") This
  is essentially a "non-greedy" version of `*`, and `.. pat` can be thought of
  as the special case of `..=. pat`

`<` *pat*
: Matches at the current position if *pat* matches immediately before the
  current position (lookbehind). **Note:** For fixed-length lookbehinds, this
  is quite efficient (e.g. `<(100 "x")`), however this can cause performance
  problems with variable-length lookbehinds (e.g. `<("x" 0-100"y")`). Also,
  patterns like `^`, `^^`, `$`, and `$$` that match against line/file edges
  will match against the edge of the lookbehind window, so they should
  generally be avoided in lookbehinds.

`>` *pat*
: Matches *pat*, but does not consume any input (lookahead).

`@` *pat*
: Capture *pat*. Captured patterns can be used in replacements.

`foo`
: The named pattern whose name is **"foo"**. Pattern names come from
  definitions in grammar files or from named captures. Pattern names may
  contain dashes (`-`), but not underscores (`_`), since the underscore is used
  to match whitespace. See the **GRAMMAR FILES** section for more info.

`@` *name* `:` *pat*
: For the rest of the current chain, define *name* to match whatever *pat*
  matches, i.e. a backreference. For example, `` @my-word:word `( my-word `) ``
  (matches **"asdf(asdf)"** or **"baz(baz)"**, but not **"foo(baz)"**)

`@` *name* `=` *pat*
: Let *name* equal *pat* (named capture). Named captures can be used in text
  replacements.

*pat* `=>` `"`*replacement*`"`
: Replace *pat* with *replacement*. Note: *replacement* should be a string
  (single or double quoted), and it may contain escape sequences (e.g. `\n`) or
  references to captured values: `@0` (the whole of *pat*), `@1` (the first
  capture in *pat*), `@`*foo* (the capture named *foo* in *pat*), etc. For
  example, `@word _ @rest=(*word % _) => "@rest:\n\t@1"` matches a word
  followed by whitespace, followed by a series of words and replaces it with
  the series of words, a colon, a newline, a tab, and then the first word.

*pat1* `~` *pat2*
: Matches when *pat1* matches and *pat2* can be found within the text of that
  match. (e.g. `comment ~ "TODO"` matches comments that contain **"TODO"**)

*pat1* `!~` *pat2*
: Matches when *pat1* matches, but *pat2* can not be found within the text of
  that match. (e.g. `comment ~ "IGNORE"` matches only comments that do not
  contain **"IGNORE"**)

*name*`:` *pat1*; *pat2*
: Define *name* to mean *pat1* (pattern definition) inside the pattern *pat2*.
  For example, a recursive pattern can be defined and used like this:
  `paren-comment: "(*" ..%paren-comment "*)"; paren-comment`

`@:`*name* `=` *pat*
: Match *pat* and tag it with the given name as metadata.

*name*`::` *pat*
: Syntactic sugar for *name*`:` `@:`*name*`=`*pat* (define a pattern that also
  attaches a metadata tag of the same name)

`#` *comment*
: A line comment, ignored by BP


# GRAMMAR FILES

**bp** allows loading extra grammar files, which define patterns which may be
used for matching. The **builtins** grammar file is loaded by default, and it
defines a few useful general-purpose patterns. For example, it defines the
**parens** rule, which matches pairs of matching parentheses, accounting for
nested inner parentheses:

```
bp 'my_func{parens}'
```

BP's builtin grammar file defines a few other commonly used patterns such as:

- `braces` (matching `{}` pairs), `brackets` (matching `[]` pairs),
  `anglebraces` (matching `<>` pairs)
- `string`: a single- or double-quote delimited string, including standard
  escape sequences
- `id` or `var`: an identifier (full UTF-8 support)
- `word`: similar to `id`/`var`, but can start with a number
- `Hex`, `hex`, `HEX`: a mixed-case, lowercase, or uppercase hex digit
- `digit`: a digit from 0-9
- `int`: one or more digits
- `number`: an int or floating point literal
- `esc`, `tab`, `nl`, `cr`, `crlf`, `lf`: Shorthand for escape sequences

**bp** also comes with a few grammar files for common programming languages,
which may be loaded on demand. These grammar files are not comprehensive syntax
definitions, but only some common patterns. For example, the c++ grammar file
contains definitions for `//`-style line comments as well as `/*...*/`-style
block comments. Thus, you can find all comments with the word "TODO" with the
following command:

```
bp -g c++ '{comment ~ "TODO"}' *.cpp
```


# EXAMPLES

Find files containing the literal string "foo.baz" (a string pattern):
```
ls | bp foo.baz
```

Find files ending with ".c" and print the name with the ".c" replaced with ".h":
```
ls | bp '.c{$}' -r '.h'
```

Find the word "foobar", followed by a pair of matching parentheses in the file
*my_file.py*:
```
bp 'foobar{parens}' my_file.py
```

Using the *html* grammar, find all *element*s matching the tag *a* in the file
*foo.html*:
```
bp -g html '{element ~ (^^"<a ")}' foo.html
```

