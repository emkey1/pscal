# C-Like Front End Overview

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

## Language Features

The language aims to resemble a tiny subset of C while remaining easy to map
to the VM:

- **Types** – `int`/`long`, `float`/`double`, `char`, `byte`, `str`, arrays
  and `struct` declarations.
- **Control flow** – `if`, `while`, `for`, `do … while` and `switch` with
  `break` and `continue`.
- **Functions** – Defined with a return type and parameter list. Pointer
  parameters allow pass‑by‑reference semantics.
- **Structs and pointers** – `struct` aggregates fields. `new(&node)` allocates
  dynamic storage and `->` dereferences pointer fields.

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

## Example: Building a Linked List

```clike
struct Node {
    int value;
    struct Node* next;
};

void push(struct Node** head, int value) {
    struct Node* n;
    new(&n);
    n->value = value;
    n->next = *head;
    *head = n;
}

void print(struct Node* head) {
    while (head) {
        write(head->value, " ");
        head = head->next;
    }
    writeln();
}

struct Node* list = NULL;
push(&list, 3);
push(&list, 1);
push(&list, 4);
print(list);           // 4 1 3
```

For tutorials and additional details, see
[`clike_tutorial.md`](clike_tutorial.md) and
[`clike_repl_tutorial.md`](clike_repl_tutorial.md). The complete
specification is available in
[`clike_language_reference.md`](clike_language_reference.md).
