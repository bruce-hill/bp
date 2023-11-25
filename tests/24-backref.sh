# With @-capturing, you can reference previous captures
# Example: bp '{@first=`a-z .. first}' matches "aba" and "xyzx", but not "abc"
bp '{@first:+Abc _ +Abc _ first}'
