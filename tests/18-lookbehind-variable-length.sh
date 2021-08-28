# Lookbehinds can have variable length.
# Example: bp -p '<(^ +`# _) "foo"' matches lines starting with "# foo", "## foo", "### foo", etc.
bp -p '<(`U +`h "...") "ok"'
