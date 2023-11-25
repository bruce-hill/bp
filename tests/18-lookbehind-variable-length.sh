# Lookbehinds can have variable length.
# Example: bp '{<(^ +`# _) "foo"}' matches lines starting with "# foo", "## foo", "### foo", etc.
bp '{<(`U +`h "...") "ok"}'
