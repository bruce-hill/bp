# The _ pattern matches zero or more spaces/tabs
# Example: bp '{`= _ "foo"}' matches "=foo", "= foo", "=  foo", etc.
bp '{"one" _ "two"}'
