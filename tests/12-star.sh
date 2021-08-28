# The star (*) prefix operator matches zero or more repetitions
# Example: bp -p '"Ha" *"ha"' will match "Ha", "Haha", "Hahaha", etc.
bp -p '`( *`x `)'
