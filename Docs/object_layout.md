# Object Layout

The runtime represents an object as a record structure where fields are
stored sequentially in memory.  Each field occupies one slot in an array
of `FieldValue` entries and can therefore be addressed by a zero‑based
index.

```
+----+----------------+-----------------+-----+
| 0  | 1              | 2               | ... |
+----+----------------+-----------------+-----+
|__vtable| field1     | field2          |     |
+----+----------------+-----------------+-----+
```

* **Slot 0** – reserved for the hidden `__vtable` pointer.
* **Inherited fields** – occupy the lowest indices after `__vtable`.
* **Declared fields** – appended in the order they appear in the class
  declaration.

Code generation uses these offsets directly which enables tooling to
predict the in‑memory layout without performing name based lookups at
runtime.
