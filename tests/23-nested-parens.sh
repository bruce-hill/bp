# parens is a pattern matching nested parentheses
# Example: bp -p '"foo" parens' matches "foo()" or "foo(baz(), qux())", but not "foo(()"
bp -p 'id parens `;'
