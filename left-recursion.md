# Left Recursion in BP

Left recursion presents a special difficulty for parsing expression grammars.
A regular recursive rule looks like this:

```
rec: "{" *rec "}"
```

This is a relatively simple case to handle, because we're always making forward
progress. In other words, calling `match(<rec>, str)` might call
`match(<rec>, str + 1)` but it will never recursively call the same function
with the same arguments: `match(<rec>, str)`.

Left-recursive rules, on the other hand, are rules that may attempt to match
themselves at the same position. For example:

```
laugh: (laugh "ha") / "Ha"
```

The rule above is functionally equivalent to `laugh: "Ha" (*"ha")`, but it's
written in a recursive style. It should be obvious by inspection that the
string `Hahaha` matches both versions of the rule, but a naive implementation
of `match()` would cause `match(<laugh>, str)` to call `match(<laugh>, str)`
before doing anything else, which results in infinite recursion and a stack
overflow. One option is to simply detect left recursion and cause a compilation
error (this is what LPEG does). However, it is possible to actually match left
recursive rules without overflowing the stack.

## Matching Left Recursion

The solution used in BP is the following:

1. Whenever matching a named rule, `foo`, against string `str`, first get the
   originally defined pattern for that rule. Call this
   `foo-original-definition`.
2. Temporarily define `match(<foo>, str) := signal left recursion and return
   Fail` (In other words, `<foo>` is temporarily defined to signal left
   recursion and then fail to match if matching against the string `str`.)
3. Run `result := match(<foo-original-definition>, str)`.
4. If the match failed, then return failure.
5. If the match was successful and did not trigger left recursion, return `result`.
6. If the match was successful, but did trigger left recursion, then:

    a. If `result` is not longer than any previous `result`, stop looping and
       return the longest `result` to avoid an infinite loop.
    b. Otherwise: temporarily define `match(<foo>, str) := signal left recursion and return <result>`
    c. Go to step 3.

This handles left recursion as a simple loop and successfully matches left
recursive patterns, even ones that are indirectly or nontrivially left
recursive.

## Example

Consider the rule `laugh: laugh "ha" / "Ha"` being applied to the input text `"Hahaha!"`:

| Temp. definition of `match(<laugh>, "Hahaha!")` | Result of `match(<laugh "ha" / "Ha">, "Hahaha!")` |
|------------------------------------------|---------------------------------------------------|
|`Fail`                                    | `Match{"Ha"}`                                     |
|`Match{"Ha"}`                             | `Match{"Haha"}`                                   |
|`Match{"Haha"}`                           | `Match{"Hahaha"}`                                 |
|`Match{"Hahaha"}`                         | `Match{"Ha"}` (Forward progress stops being made) |

As you can see in this example, each successive iteration builds up a final
match incrementally until progress is no longer being made. At that point,
the longest match is returned: `Match{"Hahaha"}`.
