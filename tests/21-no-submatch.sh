# pat1 !~ pat2 matches if pat2 cannot be found within pat1, like words *not* containing "e"
# Example: bp '{(|+`0-9|) !~ `5}' matches "123" and "678", but not "456"
bp '{word !~ `e}'
