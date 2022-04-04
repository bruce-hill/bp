# Left recursion has some tricky edge cases like this:
bp -p 'foo: (foo / `a-z) (foo / `a-z) `!; foo => "{@0}"'
