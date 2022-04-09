# The ellipsis matches text upto the following pattern, not counting newlines
# Example: bp -p '"/*" .. "*/"' matches "/* blah blah */" or "/**/"
bp -p '"hello" .. "world"'
