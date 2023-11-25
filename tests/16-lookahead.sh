# >pat is a lookahead
# Example: bp '{"foo" >`(}' will match "foo" only when it is followed by a parenthesis
bp '{>`t word}'
