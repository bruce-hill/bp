# The '%' operator modifies repeating patterns, allowing you to give a separator between matches
# Example: bp '{+"x" % ":"}' will match "x", "x:x", "x:x:x", etc.
bp '{`( +int % `, `)}'
