# pat1 ~ pat2 matches if pat2 can be found within pat1, like words containing "e"
# Example: bp -p '+`0-9 ~ `5' matches "12345" and "72581", but not "789"
bp -p 'word ~ `e'
