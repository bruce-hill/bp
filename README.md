# BPEG

BPEG is a parsing expression grammar tool for the command line.
It's written in pure C with no dependencies.

## Usage
`bpeg [flags] <pattern> [<input files>...]`

### Flags
* `-h` `--help` print the usage and quit
* `-v` `--verbose` print verbose debugging info
* `-i` `--ignore-case` perform a case-insensitive match
* `-d` `--define <name>:<def>` define a grammar rule
* `-D` `--define-string <name>:<def>` define a grammar rule (string-pattern)
* `-p` `--pattern <pat>` provide a pattern (equivalent to bpeg '
* `-P` `--pattern-string <pat>` provide a string pattern (equivalent to bpeg '<pat>', but may be useful if '<pat>' begins with a '-')
* `-r` `--replace <replacement>`   replace the input pattern with the given replacement
* `-m` `--mode <mode>` set the behavior mode (defult: find-all)
* `-g` `--grammar <grammar file>`  use the specified file as a grammar

See `man ./bpeg.1` for more details.

## BPEG Patterns
BPEG patterns are a mixture of Parsing Expression Grammar and Regular
Expression syntax, with a preference for prefix operators instead of
suffix operators.

See `man ./bpeg.1` for more details.

## License
BPEG is provided under the MIT license with the Commons Clause (see
[LICENSE](LICENSE) for details).
