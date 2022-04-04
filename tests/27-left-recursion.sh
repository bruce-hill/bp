# Left recursion should work
bp -p 'xys: (xys / `x) `y; xys => "{@0}"'
