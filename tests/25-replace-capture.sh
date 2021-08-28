# Captures with @ can be referenced in a replacement by @1, @2, etc.
# Example: bp -p '"=" _ @+`0-9 => "= -@1"' replaces "x = 5" with "x = -5"
# Note: @0 refers to the entire match, e.g. bp -p '"foo" => "xx@0xx"' replaces "foo" with "xxfooxx"
bp -p '@`a,e,i,o,u => "{@1}" / .'
