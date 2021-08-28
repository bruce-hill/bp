# !pat matches only if pat doesn't match
# Example: bp -p '"cat" !"aclysm"' matches the "cat" in "catatonic", but not "cataclysm"
bp -p '"foo" !"bar"'
