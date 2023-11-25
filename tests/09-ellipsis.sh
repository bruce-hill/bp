# The ellipsis matches text upto the following pattern, not counting newlines
# Example: bp '{"/*" .. "*/"}' matches "/* blah blah */" or "/**/"
bp '{"hello" .. "world"}'
