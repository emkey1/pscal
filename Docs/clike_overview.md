# C-Like Front End Overview

The `clike` front end provides a C-style language that targets the
Pscal virtual machine.

## Core Semantics

- **Array subscripts are zero-based** – Arrays start at index 0; the first
  element of `a` is `a[0]`.
- **String indexing is 1-based** – Characters in a `str` start at index 1
  (`s[1]` is the first character). Accessing `s[0]` returns the length of the
  string as an integer.
- **String literals are immutable** – Use `copy` to obtain a writable
  string before modifying characters in place.
- **`str` values are copied on call** – Strings are passed by value. To
  mutate a caller's string, pass a pointer (e.g. `str*`) and write back to
  it after making changes.
- **Concatenation with `+`** – The `+` operator joins strings, e.g.
  `buffer = mstreambuffer(ms) + "\n";`.
- **Standard string helpers** – Built-ins such as `strlen`, `copy` and
  `upcase` operate on `str` values.
- **Dynamic allocation** – Use `new(&node);` to allocate structures;
  fields are accessed with the usual `->` syntax.

## Language Features

The language aims to resemble a tiny subset of C while remaining easy to map
to the VM:

- **Types** – `byte`, `int`, `long`, `long long`, `float`, `double`,
  `long double`, `char`, `str`, `text`, `mstream`, arrays and `struct`
  declarations.
- **Control flow** – `if`, `while`, `for`, `do … while` and `switch` with
  `break` and `continue`.
- **Functions** – Defined with a return type and parameter list. Pointer
  parameters allow pass‑by‑reference semantics.
- **Structs and pointers** – `struct` aggregates fields. `new(&node)` allocates
  dynamic storage and `->` dereferences pointer fields.
- **Threading and Synchronization** – `spawn` launches a parameterless function in a new thread and returns its id; `join` waits for a thread to complete; `mutex`/`rcmutex` create standard or recursive mutexes and return ids, and `lock`/`unlock` guard critical sections.

## Example: Sorting a String

```clike
void sort_string(str* sp) {
    int i, j, len;
    char tmp;
    str s = *sp;

    len = strlen(s); // strings index from one
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
    *sp = s;
}

void main() {
    // String literals are immutable; make a writable copy first.
    str sort = copy("This is a string", 1, strlen("This is a string"));
    sort_string(&sort);
    printf("Sorted String is %s\n", sort);
}

```

## Example: Building a Linked List

```clike
struct Node {
    int value;
    struct Node* next;
};

struct Node* push(struct Node* head, int value) {
    struct Node* n;
    new(&n);
    n->value = value;
    n->next = head;
    return n;
}

void print(struct Node* head) {
    while (head != NULL) {
        printf("%d ", head->value);
        head = head->next;
    }
    printf("\n");
}

void main() {
    struct Node* list = NULL;
    list = push(list, 3);
    list = push(list, 1);
    list = push(list, 4);
    print(list);           // 4 1 3
}

```

For tutorials and additional details, see
[`clike_tutorial.md`](clike_tutorial.md) and
[`clike_repl_tutorial.md`](clike_repl_tutorial.md). The complete
specification is available in
[`clike_language_reference.md`](clike_language_reference.md).
