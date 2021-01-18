# BP Grammars

The files in this directory are predefined grammars for different languages and
contexts. They are intended to be used for common search patterns, and **not**
intended to be complete PEG definitions of language grammars, other than
[bp.bp](./bp.bp), which is included for stress-testing purposes, as well as a
showcase of some BP features.

## Adding Grammars

If you want to add your own grammar, the easiest way to do so is to create a
`.bp` file in `~/.config/bp/`. The syntax for grammar files is fully and
formally defined in [bp.bp](./bp.bp), but in short, it's a list of
whitespace-separated rule definitions of the form `id __ ":" __ pattern`.
