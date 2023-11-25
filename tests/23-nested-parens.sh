# parens is a pattern matching nested parentheses
# Example: bp '{"foo" parens}' matches "foo()" or "foo(baz(), qux())", but not "foo(()"
bp '{id parens `;}'
