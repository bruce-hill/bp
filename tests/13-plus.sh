# The plus (+) prefix operator matches one or more of a pattern
# Example: bp -p '"l" +"ol"' will match "lol", "lolol", "lololol", etc.
bp -p '`( +`x `)'
