local bp = require 'bp'

local function iter(state, _)
    local m, start, len = bp.match(state[1], state[2], state[3])
    state[3] = m and start+math.max(len,1)
    return m, start, len
end

bp.each = function(s, pat, index)
    return iter, {s, pat, index}, index
end

local function repr(obj)
    if type(obj) == 'table' then
        local ret = {}
        for k,v in pairs(obj) do table.insert(ret, ("%s=%s"):format(k, repr(v))) end
        return ("{%s}"):format(table.concat(ret,","))
    elseif type(obj) == 'string' then
        return string.format("%q", obj)
    else
        return tostring(obj)
    end
end

print("Matching:")
for m, i,j in bp.each("one two  three", "(*`a-z) => '(@0)'") do
    print(("%s @%d len=%d"):format(repr(m),i,j))
end

print(("Replacing: %q (%d replacements)"):format(bp.replace("one two  three", "+`a-z", "(@0)")))


print("Captures:")
local m = bp.match("one two three four", "@first=+`a-z _ @second=(+`a-z => 'XX@0XX') _ @+`a-z _ @last=+`a-z")
print(repr(m))

local m = bp.match("one two three four", "@dup=+`a-z _ @dup=+`a-z _ @two=(@a=+`a-z _ @b=+`a-z)")
print(repr(m))


print("Testing parse errors:")
local ok, msg = pcall(function()
    bp.match("xxx", ".;//;;; wtf")
end)
if not ok then print(("\x1B[41;30mParse error:\x1B[0;1;31m %s\x1B[m\n"):format(msg)) end

print("Testing builtins:")
print(bp.match("...(foo())...", "parens"))
