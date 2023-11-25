# The ordered choice operator (/) picks the first choice that matches
# Example: bp '{"cabaret"/"cab"}' matches either "cabaret" or "cab"
# Note: if a match occurs, the options to the right will *never* be attempted,
# so bp '{"cab"/"cabaret"}' will always match "cab" instead of "cabaret"
bp '{"foo" / "bar"}'
