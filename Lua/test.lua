local bp = require 'bp'

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
print(bp.match(".", "ABC"))
print(bp.match(".", "ABC", 2))
print(bp.match(".", "ABC", 3))
print(bp.match(".", "ABC", 4) or "no match")
print(bp.match(".", "ABC", 5) or "no match")

for m in bp.matches("(*`a-z) => '(@0)'", "one two  three") do
    print(repr(m))
end

print(("Replacing: %q (%d replacements)"):format(bp.replace("+`a-z", "(@0)", "one two  three")))


print("Captures:")
local m = bp.match("@first=+`a-z _ @second=(+`a-z => 'XX@0XX') _ @+`a-z _ @last=+`a-z", "one two three four")
print(repr(m))

local m = bp.match("@dup=+`a-z _ @dup=+`a-z _ @two=(@a=+`a-z _ @b=+`a-z)", "one two three four")
print(repr(m))


print("Testing parse errors:")
local ok, msg = pcall(function()
    bp.match(".;//;;; wtf", "xxx")
end)
if not ok then print(("\x1B[41;30mParse error:\x1B[0;1;31m %s\x1B[m\n"):format(msg)) end

print("Testing builtins:")
print(bp.match("parens", "...(foo())..."))


print("Testing pat objects")
local pat = bp.compile("+`a-z")
print(pat)
print(pat:match("...foo..."))
print(pat:match("...baz..."))
print(pat:replace("{@0}", "...baz..."))

for m in pat:matches("hello world") do
    print(m)
end
