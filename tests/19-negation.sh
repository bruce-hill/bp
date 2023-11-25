# !pat matches only if pat doesn't match
# Example: bp '"cat" !"aclysm"' matches the "cat" in "catatonic", but not "cataclysm"
bp '{"foo" !"bar"}'
