# CLike Front End Overview

The `clike` front end provides a small C-style language that targets the
Pscal virtual machine.

## Core Semantics

- **Array subscripts are zero-based** – Arrays start at index 0; the first
  element of `a` is `a[0]`.
- **String indexing is 1-based** – Characters in a `str` start at index 1
  (`s[1]` is the first character). Accessing `s[0]` returns the length of the
  string as an integer.
- **`str` values are pointers** – A `str` variable already evaluates to a
  pointer to its character buffer.  Pass the variable directly to
  functions; taking an additional address yields a pointer-to-pointer and
  confuses element access.
- **Concatenation with `+`** – The `+` operator joins strings, e.g.
  `buffer = mstreambuffer(ms) + "\n";`.
- **Standard string helpers** – Built-ins such as `strlen`, `copy` and
  `upcase` operate on `str` values.
- **Dynamic allocation** – Use `new(&node);` to allocate structures;
  fields are accessed with the usual `->` syntax.

## Example: Sorting a String

```clike
void sort_string(str s) {
    int i, j, len;
    char tmp;
    len = strlen(s);
    i = 1;
    while (i <= len) {
        j = i + 1;
        while (j <= len) {
            if (s[i] > s[j]) {
                tmp = s[i];
                s[i] = s[j];
                s[j] = tmp;
            }
            j++;
        }
        i++;
    }
}

str guessed = "ST";
sort_string(guessed);  // pass the string value, not its address
```

For tutorials and additional details, see
[`clike_tutorial.md`](clike_tutorial.md) and
[`clike_repl_tutorial.md`](clike_repl_tutorial.md).
