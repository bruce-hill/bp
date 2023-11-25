# The plus (+) prefix operator matches one or more of a pattern
# Example: bp '{"l" +"ol"}' will match "lol", "lolol", "lololol", etc.
bp '{`( +`x `)}'
