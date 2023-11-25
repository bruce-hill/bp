# Captures with @ can be referenced in a replacement by @1, @2, etc.
# Example: bp '{"=" _ @+`0-9 => "= -@1"}' replaces "x = 5" with "x = -5"
# Note: @0 refers to the entire match, e.g. bp '{"foo" => "xx@0xx"}' replaces "foo" with "xxfooxx"
bp '{@`a,e,i,o,u => "{@1}" / .}'
