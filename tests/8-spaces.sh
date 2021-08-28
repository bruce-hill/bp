# The _ pattern matches zero or more spaces/tabs
# Example: bp -p '`= _ "foo"' matches "=foo", "= foo", "=  foo", etc.
bp -p '"one" _ "two"'
