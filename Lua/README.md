# Lua Bindings

This directory contains Lua bindings for bp.

## API

```lua
local bp = require("bp")
local m, i, len = bp.match("like finding a needle in a haystack", '"n" +`e "dle"')
local replaced = bp.match("like finding a needle in a haystack", '"n" +`e "dle"', "cat")
```
