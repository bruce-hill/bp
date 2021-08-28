# The '%' operator modifies repeating patterns, allowing you to give a separator between matches
# Example: bp -p '+"x" % ":"' will match "x", "x:x", "x:x:x", etc.
bp -p '`( +int % `, `)'
