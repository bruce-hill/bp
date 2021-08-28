# >pat is a lookahead
# Example: bp -p '"foo" >`(' will match "foo" only when it is followed by a parenthesis
bp -p '>`t word'
