# With @-capturing, you can reference previous captures
# Example: bp -p '@first=`a-z .. first' matches "aba" and "xyzx", but not "abc"
bp -p '@first=+Abc _ +Abc _ first'
