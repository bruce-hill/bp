# Lua Bindings

This directory contains Lua bindings for bp.

## API

```lua
local bp = require("bp")
local m, i, len = bp.match("like finding a needle in a haystack", '"n" @Es=+`e "dle"')
--> {[0]="needle", Es={[0]="ee"}}
--> tostring(m) == "needle", tostring(m.Es) == "ee"
local replaced = bp.match("like finding a needle in a haystack", '"n" +`e "dle"', "cat")
--> "like finding a cat in a haystack"
```
