# Replacements can be done with pat => "replacement"
# Example: bp -p '"foo" => "baz"' matches "foobar" and replaces it with "bazbar"
bp -p '"s" => "$"'
