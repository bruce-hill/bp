# Left recursion has some tricky edge cases like this:
bp '{shout: phrase `!; phrase: shout / id; phrase => "{@0}"}'
