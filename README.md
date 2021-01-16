# BP - Bruce's PEG Tool

BP is a parsing expression grammar (PEG) tool for the command line.
It's written in pure C with no dependencies.

## Usage
`bp [flags] <pattern> [<input files>...]`

### Flags
* `-h` `--help` print the usage and quit
* `-v` `--verbose` print verbose debugging info
* `-i` `--ignore-case` perform a case-insensitive match
* `-I` `--inplace` perform replacements or filtering in-place on files
* `-C` `--confirm` during replacement, confirm before each replacement
* `-e` `--explain` print an explanation of the matches
* `-j` `--json` print matches as JSON objects
* `-l` `--list-files` print only filenames containing matches
* `-p` `--pattern <pat>` provide a pattern (equivalent to `bp '\(<pat>)'`)
* `-P` `--pattern-string <pat>` provide a string pattern (equivalent to `bp '<pat>'`, but may be useful if `'<pat>'` begins with a '-')
* `-r` `--replace <replacement>`   replace the input pattern with the given replacement
* `-c` `--context <N>` change how many lines of context are printed (`0`: no context, `all`: the whole file, `<N>` matching lines and `<N-1>` lines before/after)
* `-g` `--grammar <grammar file>`  use the specified file as a grammar

See `man ./bp.1` for more details.

## BP Patterns
BP patterns are a mixture of Parsing Expression Grammar and Regular
Expression syntax, with a preference for prefix operators instead of
suffix operators.

Pattern            | Meaning
-------------------|---------------------
`pat1 pat2`        | `pat1` followed by `pat2`
`pat1 / pat2`      | `pat1` if it matches, otherwise `pat2`
`..pat`            | Any text up to and including `pat` (except newlines)
`..pat1 % pat2`    | Any text up to and including `pat1` (except newlines), skipping over instances of `pat2`
`.`                | Any single character (except newline)
`^^`               | The start of the input
`^`                | The start of a line
`$$`               | The end of the input
`$`                | The end of a line
`__`               | Zero or more whitespace characters (including newlines)
`_`                | Zero or more whitespace characters (excluding newlines)
`` `c ``           | The literal character `c`
`` `a-z ``         | The character range `a` through `z`
`` `a,b ``         | The character `a` or the character `b`
`\n`, `\033`, `\x0A`, etc. | An escape sequence character
`\x00-xFF`         | An escape sequence range (byte `0x00` through `0xFF` here)
`!pat`             | `pat` does not match at the current position
`[pat]` or `pat?`  | Zero or one occurrences of `pat` (optional pattern)
`5 pat`            | Exactly 5 occurrences of `pat`
`2-4 pat`          | Between 2 and 4 occurrences of `pat` (inclusive)
`5+ pat`           | 5 or more occurrences of `pat`
`5+ pat % sep`     | 5 or more occurrences of `pat`, separated by `sep` (e.g. `0+ int % ","` matches `1,2,3`)
`*pat`             | 0 or more occurrences of `pat` (shorthand for `0+pat`)
`+pat`             | 1 or more occurrences of `pat` (shorthand for `1+pat`)
`<pat`             | `pat` matches just before the current position (backref)
`>pat`             | `pat` matches just in front of the current position (lookahead)
`@pat`             | Capture `pat` (used for text replacement and backreferences)
`@foo=pat`         | Let `foo` be the text of `pat` (used for text replacement and backreferences)
`pat => "replacement"` | Match `pat` and replace it with `replacement`
`(pat1 @keep=pat2) => "@keep"` | Match `pat1` followed by `pat2` and replace it with the text of `pat2`
`pat1==pat2`       | `pat1`, assuming `pat2` also matches with the same length
`pat1!=pat2`       | `pat1`, unless `pat2` also matches with the same length
`name:pat2`        | `name` is defined to mean `pat`
`#( block comment )#` | A block comment
`# line comment`   | A line comment

See `man ./bp.1` for more details.

## License
BP is provided under the MIT license with the [Commons Clause](https://commonsclause.com/)
(you can't sell this software without the developer's permission, but you're
otherwise free to use, modify, and redistribute it free of charge).
See [LICENSE](LICENSE) for details.
