# Left recursion has some tricky edge cases like this:
bp -p 'shout: phrase `!; phrase: shout / id; phrase => "{@0}"'
