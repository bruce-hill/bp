% BP(1)
% Bruce Hill (*bruce@bruce-hill.com*)
% May 17 2021

# NAME

bp - Bruce\'s Parsing Expression Grammar tool

# SYNOPSIS

**bp**
\[*options...*\]
*pattern*
\[\[\--\] *files...*\]

# DESCRIPTION

**bp** is a tool that matches parsing expression grammars using a custom
syntax.

# OPTIONS

**-v**, **\--verbose**
: Print debugging information.

**-e**, **\--explain**
: Print a visual explanation of the matches.

**-j**, **\--json**
: Print a JSON list of the matches. (Pairs with **\--verbose** for more detail)

**-l**, **\--list-files**
: Print only the names of files containing matches instead of the matches
themselves.

**-i**, **\--ignore-case**
: Perform pattern matching case-insensitively.

**-I**, **\--inplace**
: Perform filtering or replacement in-place (i.e. overwrite files with new
content).

**-C**, **\--confirm**
: During in-place modification of a file, confirm before each modification.

**-r**, **\--replace** *replacement*
: Replace all occurrences of the main pattern with the given string.

**-s**, **\--skip** *pattern*
: While looking for matches, skip over *pattern* occurrences. This can be
useful for behavior like **bp -s string** (avoiding matches inside string
literals).

**-g**, **\--grammar** *grammar-file*
: Load the grammar from the given file. See the **GRAMMAR FILES** section
for more info.

**-G**, **\--git**
: Use **git** to get a list of files. Remaining file arguments (if any) are
passed to **git \--ls-files** instead of treated as literal files.

**-c**, **\--context** *N*
: The number of lines of context to print. If *N* is 0, print only the
exact text of the matches. If *N* is **`"all"`**, print the entire file.
Otherwise, if *N* is a positive integer, print the whole line on which
matches occur, as well as the *N-1* lines before and after the match. The
default value for this argument is **1** (print whole lines where matches
occur).

**-f**, **\--format** *auto*\|*fancy*\|*plain*
: Set the output format. *fancy* includes colors and line numbers, *plain*
includes neither, and *auto* (the default) uses *fancy* formatting only when
the output is a TTY.

**\--help**
: Print the usage and exit.

*pattern*
: The main pattern for bp to match. By default, this pattern is a string
pattern (see the **STRING PATTERNS** section below).

*files...*
: The input files to search. If no input files are provided and data was piped
in, that data will be used instead. If neither are provided, **bp** will search
through all files in the current directory and its subdirectories
(recursively).


# STRING PATTERNS

One of the most common use cases for pattern matching tools is matching plain,
literal strings, or strings that are primarily plain strings, with one or two
patterns. **bp** is designed around this fact. The default mode for bp patterns
is "string pattern mode". In string pattern mode, all characters are
interpreted literally except for the backslash (**\\**), which may be followed
by a bp pattern (see the **PATTERNS** section above). Optionally, the bp
pattern may be terminated by a semicolon (**;**).


# PATTERNS

**bp** patterns are based off of a combination of Parsing Expression Grammars
and regular expression syntax. The syntax is designed to map closely to verbal
descriptions of the patterns, and prefix operators are preferred over suffix
operators (as is common in regex syntax).

Some patterns additionally have "multi-line" variants, which means that they
include the newline character.

*pat1 pat2*
: A sequence: *pat1* followed by *pat2*

*pat1* **/** *pat2*
: A choice: *pat1*, or if it doesn\'t match, then *pat2*

**.**
: Any character (excluding newline)

**\^**
: Start of a line

**\^\^**
: Start of the text

**\$**
: End of a line (does not include newline character)

**\$\$**
: End of the text

**\_**
: Zero or more whitespace characters, including spaces and tabs, but not
newlines.

**\_\_**
: Zero or more whitespace characters, including spaces, tabs, newlines, and
comments. Comments are undefined by default, but may be defined by a separate
grammar file. See the **GRAMMAR FILES** section for more info.

**\"foo\"**, **\'foo\'**
: The literal string **"foo"**. Single and double quotes are treated the same.
Escape sequences are not allowed.

**{foo}**
: The literal string **"foo"** with word boundaries on either end. Escape
sequences are not allowed.

**\`***c*
: The literal character *c* (e.g. **\`@** matches the "@" character)

**\`***c1***,***c2*
: The literal character *c1* or *c2* (e.g. **\`a,e,i,o,u**)

**\`***c1***-***c2*
: The character range *c1* to *c2* (e.g. **\`a-z**). Multiple ranges
can be combined with a comma (e.g. **\`a-z,A-Z**).

**\\***esc*
: An escape sequence (e.g. **\\n**, **\\x1F**, **\\033**, etc.)

**\\***esc1***-***esc2*
: An escape sequence range from *esc1* to *esc2* (e.g. **\\x00-x1F**)

**\\N**
: A special case escape that matches a "nodent": one or more newlines followed
by the same indentation that occurs on the current line.

**!** *pat*
: Not *pat*

**\[** *pat* **\]**
: Maybe *pat*

*N* *pat*
: Exactly *N* repetitions of *pat* (e.g. **5 \`x** matches **"xxxxx"**)

*N* **-** *M* *pat*
: Between *N* and *M* repetitions of *pat* (e.g. **2-3 \`x**
matches **"xx"** or **"xxx"**)

*N***+** *pat*
: At least *N* or more repetitions of *pat* (e.g. **2+ \`x** matches
**"xx"**, **"xxx"**, **"xxxx"**, etc.)

**\*** *pat*
: Some *pat*s (zero or more, e.g. **\* \`x** matches **""**, **"x"**,
**"xx"**, etc.)

**+** *pat*
: At least one *pat*s (e.g. **\+ \`x** matches **"x"**, **"xx"**,
**"xxx"**, etc.)

*repeating-pat* **%** *sep*
: *repeating-pat* separated by *sep* (e.g. **\*word % \`,** matches
zero or more comma-separated words)

**..** *pat*
: Any text (except newlines) up to and including *pat*

**.. %** *skip* *pat*
: Any text (except newlines) up to and including *pat*, skipping over
instances of *skip* (e.g. **\`\"..\`\" % (\`\\.)**)

**\<** *pat*
: Matches at the current position if *pat* matches immediately before the
current position (lookbehind). Conceptually, you can think of this as creating
a file containing only the *N* characters immediately before the current
position and attempting to match *pat* on that file, for all values of *N* from
the minimum number of characters *pat* can match up to maximum number of
characters *pat* can match (or the length of the current line upto the current
position, whichever is smaller). **Note:** For fixed-length lookbehinds, this
is quite efficient (e.g. **\<(100\`x)**), however this could cause performance
problems with variable-length lookbehinds (e.g. **\<(\`x 0-100\`y)**). Also,
it is not advised to use **\^**, **\^\^**, **$**, or **$$** inside a lookbehind,
as they will match against the edges of the lookbehind slice.

**\>** *pat*
: Matches *pat*, but does not consume any input (lookahead).

**\@** *pat*
: Capture *pat*

**foo**
: The named pattern whose name is **"foo"**. Pattern names come from definitions in
grammar files or from named captures. Pattern names may contain dashes (**-**),
but not underscores (**\_**), since the underscore is used to match whitespace.
See the **GRAMMAR FILES** section for more info.

**\@** *name* **=** *pat*
: Let *name* equal *pat* (named capture). Named captures can be used as
backreferences like so: **\@foo=word \`( foo \`)** (matches **"asdf(asdf)"** or
**"baz(baz)"**, but not **"foo(baz)"**)

*pat* **=\> \'***replacement***\'**
: Replace *pat* with *replacement*. Note: *replacement* should be a
string, and it may contain references to captured values: **\@0** (the whole of
*pat*), **\@1** (the first capture in *pat*), **\@***foo* (the capture
named *foo* in *pat*), etc. For example, **\@word \_ \@rest=(\*word % \_)
=\> \"\@rest \@1\"**

*pat1* **~** *pat2*
: Matches when *pat1* matches and *pat2* can be found within the text of that
match. (e.g. **comment ~ {TODO}** matches comments that contain the word
**"TODO"**)

*pat1* **!~** *pat2*
: Matches when *pat1* matches, but *pat2* can not be found within the text of
that match. (e.g. **comment ~ {IGNORE}** matches only comments that do not
contain the word **"IGNORE"**)

*name***:** *pat*
: Define *name* to mean *pat* (pattern definition)

**(!)** *error-pat*
: If *error-pat* matches, **bp** will not print any results in this file and
instead print an error message to **STDERR** highlighting the matching position
of *error-pat* in the file and printing the text of *error-pat* as an error
message. Then, **bp** will exit with a failure status and not process any
further files.

**\#** *comment*
: A line comment


# GRAMMAR FILES

**bp** allows loading extra grammar files, which define patterns which may be
used for matching. The **builtins** grammar file is loaded by default, and it
defines a few useful general-purpose patterns. For example, it defines the
**parens** rule, which matches pairs of matching parentheses, accounting for
nested inner parentheses:

```
bp -p '"my_func" parens'
```

**bp** also comes with a few grammar files for common programming languages,
which may be loaded on demand. These grammar files are not comprehensive syntax
definitions, but only some common patterns. For example, the c++ grammar file
contains definitions for **//**-style line comments as well as
**/\*...\*/**-style block comments. Thus, you can find all comments with the
word "TODO" with the following command:

```
bp -g c++ -p 'comment~{TODO}' *.cpp
```


# EXAMPLES

**ls \| bp foo**
: Find files containing the string \"foo\" (a string pattern)

**ls \| bp \'.c\\\$\' -r \'.h\'**
: Find files ending with \".c\" and replace the extension with \".h\"

**bp -p \'{foobar} parens\' my_file.py**
: Find the word **\"foobar\"**, followed by a pair of matching parentheses in
the file *my_file.py*

**bp -g html -p \'element ~ (^^\"\<a \")\' foo.html**
: Using the *html* grammar, find all *element*s matching the tag *a* in the
file *foo.html*

**bp -g python -p \'comment~{TODO}\' \*.py**
: Find all comments with the word **"TODO"** in local python files.
