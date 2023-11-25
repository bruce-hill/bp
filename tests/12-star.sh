# The star (*) prefix operator matches zero or more repetitions
# Example: bp '{"Ha" *"ha"}' will match "Ha", "Haha", "Hahaha", etc.
bp '{`( *`x `)}'
