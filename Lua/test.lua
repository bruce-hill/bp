local bp = require 'bp'

local function iter(state, _)
    local m, start, len = bp.match(state[1], state[2], state[3])
    state[3] = m and start+math.max(len,1)
    return m, start, len
end

bp.each = function(s, pat, index)
    return iter, {s, pat, index}, index
end

print("Matching:")
for m, i,j in bp.each("one two  three", "(*`a-z) => '(@0)'") do
    print(("%q @%d len=%d"):format(tostring(m),i,j))
end

print(("Replacing: %q (%d replacements)"):format(bp.replace("one two  three", "+`a-z", "(@0)")))


print("Captures:")
local m = bp.match("one two three four", "_:` ;@first=+`a-z _ @second=(+`a-z => 'XX@0XX') _ @+`a-z _ @last=+`a-z")
local function quote(x) return ("%q"):format(tostring(x)) end
print("0", quote(m[0]))
print("first", quote(m.first))
print("second", m.second)
print("1", m[1])
print("last", m.last)

print("Len:", #m, #tostring(m))


print("Testing parse errors:")
local ok, msg = pcall(function()
    bp.match("xxx", ".;//;;; wtf")
end)
if not ok then print(("\x1B[41;30mParse error:\x1B[0;1;31m %s\x1B[m\n"):format(msg)) end
