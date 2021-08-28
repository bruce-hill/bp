# Multiple patterns in a row represent a sequence.
# bp pattern syntax mostly doesn't care about whitespace, so you can have
# spaces between patterns if you want, but it's not required.
# Example: bp -p '"foo" `0-9' matches "foo1", "foo2", etc.
bp -p '"one" "two"'
