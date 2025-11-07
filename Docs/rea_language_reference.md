# Rea Language Specification

### **Introduction and Design Philosophy**

Rea is an object‑oriented front end for the PSCAL virtual machine (VM).  It
balances readability with direct access to the VM's stack‑based runtime,
bridging the gap between the Pascal and C‑Like syntaxes with a concise,
class‑centric language.  The design emphasizes simple, explicit semantics and a
single dispatch model so that compiled programs map cleanly onto PSCAL
bytecode.

### **Lexical Structure**

#### **Comments**

* `//` introduces a single-line comment. When `//` appears between two expressions it acts as the integer-division operator instead.
  Place it at the start of a statement (or after punctuation such as `;` or `{`) to keep the comment form.
* `/* ... */` encloses a multi-line comment.

#### **Identifiers**

Identifiers are case‑sensitive.  They must begin with a letter or `_` and may be
followed by letters, digits, or underscores.

#### **Keywords**

Reserved words may not be used as identifiers.

* **Types:** `int`, `int32`, `int16`, `int8`, `float`, `float32`, `long double`,
  `str`, `bool`, `void`
* **Classes:** `class`, `new`, `extends`, `my`, `myself`, `super`
* **Control flow:** `if`, `else`, `while`, `for`, `do`, `switch`, `case`,
  `default`, `break`, `continue`, `return`
* **Other:** `const`, `#import`

#### **Literals**

* **Integer and floating point:** follow C‑style forms with optional prefixes
  and exponents.  Unsuffixed numbers are 64‑bit signed integers or doubles.
* **Character:** single quotes with C escape sequences, e.g. `'\n'`, `'\x41'`.
* **String:** double quotes with the same escape sequences, e.g. "hello".

### **Data Types**

Rea reuses the VM's primitive types.  By default integers are 64‑bit and floats
are 64‑bit doubles.

| Rea Keyword | VM Type | Description |
| :--- | :--- | :--- |
| `int` | `TYPE_INT64` | 64‑bit signed integer |
| `int32` | `TYPE_INT32` | 32‑bit signed integer |
| `int16` | `TYPE_INT16` | 16‑bit signed integer |
| `int8` | `TYPE_INT8` | 8‑bit signed integer |
| `float` | `TYPE_DOUBLE` | 64‑bit floating point |
| `float32` | `TYPE_FLOAT` | 32‑bit floating point |
| `long double` | `TYPE_LONG_DOUBLE` | Extended precision float |
| `str` | `TYPE_STRING` | Dynamically sized string |
| `bool` | `TYPE_BOOLEAN` | `true` or `false` |
| `void` | `TYPE_VOID` | No value (procedures) |

### **Variables and Constants**

Variables must be declared with a type.  Constants use `const` and require a
compile‑time value.

```rea
int x;
const int LIMIT = 10;
```

### **Expressions and Operators**

Rea supports the common arithmetic and comparison operators `+ - * / // % == != <
<= > >=` plus logical `&&` and `||`.  Assignment forms such as `+=` and `-=` are
also available.  Operator precedence mirrors that of C.

### **Statements**

* **Expression statement:** `expr;`
* **Block:** `{ statement* }`
* **`if` / `else`:** standard conditional selection
* **Loops:** `while`, `do … while`, and `for` loops
* **`switch`:** multi‑way branch with `case` and `default`
* **`break` / `continue`:** loop control
* **`return`:** exit a function and optionally yield a value

### **Functions and Methods**

Functions declare a return type followed by a name and parameter list.  Methods
follow the same pattern but are nested inside classes.

```rea
int add(int a, int b) { return a + b; }
```

### **Classes and Objects**

* **Definition:** `class Name { field; method() { … } }`
* **Fields:** declared like variables; each instance holds its own copy.
* **Methods:** may access instance members through `my` or `myself`.
* **Constructors:** a method sharing the class name initializes new objects.
* **Instantiation:** `new Name(args)` allocates an object and calls the
  constructor.
* **Inheritance:** `class B extends A { … }` creates a subclass.
* **`super`:** accesses the parent class constructor or overridden methods.
* **Dispatch:** all non‑static methods are virtual and resolved via per‑class
  V‑tables.

### **Built‑in Routines**

Rea code can call any PSCAL VM built‑in, including I/O (`writeln`, `printf`),
string helpers, math functions, and threading primitives such as `spawn`,
`join`, `mutex`, `lock`, and `unlock`.

When PSCAL is compiled with SDL support, the same graphics and audio helpers used by Pascal are available to Rea programs. The 3D layer includes `InitGraph3D`, `GLSwapWindow`, `GLSetSwapInterval`, and a collection of fixed-function helpers (`GLClearColor`, `GLClear`, `GLClearDepth`, `GLMatrixMode`, `GLLoadIdentity`, `GLTranslatef`, `GLRotatef`, `GLScalef`, `GLPerspective`, `GLFrustum`, `GLBegin`/`GLEnd`, `GLColor3f`, `GLColor4f`, `GLVertex3f`, `GLNormal3f`, `GLLineWidth`, `GLEnable`, `GLDisable`, `GLCullFace`, `GLShadeModel`, `GLLightfv`, `GLMaterialfv`, `GLMaterialf`, `GLColorMaterial`, `GLBlendFunc`, `GLViewport`, `GLDepthTest`, ...). A short render loop looks like:

```rea
int main() {
  bool vsyncOn = true;
  InitGraph3D(640, 480, "Swap Demo", 24, 8);
  GLViewport(0, 0, 640, 480);
  GLClearDepth(1.0);
  GLDepthTest(true);
  GLSetSwapInterval(1);

  for (int frame = 0; frame < 600; frame = frame + 1) {
    GLClearColor(0.1, 0.1, 0.15, 1.0);
    GLClear();

    GLMatrixMode("modelview");
    GLLoadIdentity();
    GLRotatef(frame * 0.5, 0.0, 1.0, 0.0);

    GLBegin("triangles");
      GLColor3f(1.0, 0.0, 0.0);
      GLVertex3f(0.0, 0.5, 0.0);
      GLColor3f(0.0, 1.0, 0.0);
      GLVertex3f(-0.5, -0.5, 0.0);
      GLColor3f(0.0, 0.0, 1.0);
      GLVertex3f(0.5, -0.5, 0.0);
    GLEnd();

    if (frame > 0 && frame % 120 == 0) {
      vsyncOn = !vsyncOn;
      GLSetSwapInterval(vsyncOn ? 1 : 0);
    }
    GLSwapWindow();
    GraphLoop(1);
  }

  CloseGraph3D();
  return 0;
}
```

The repository includes `Examples/rea/sdl/multibouncingballs_3d`, which expands
this into a full 3D physics demo with per-face colors, depth testing, and
camera controls. `Examples/rea/sdl/landscape` builds on the same helpers to
generate a seeded terrain height field and lets you roam it with `IsKeyDown`
and `GLPerspective` for a first-person view.

### **Example**

```rea
class Counter {
  int n;
  void inc() { my.n = my.n + 1; }
  int set(int v) { my.n = v; return my.n; }
}

Counter c = new Counter();
c.set(3);
writeln("c=", c.n);
c.inc();
writeln("c=", c.n);
```

This program demonstrates field updates and virtual method calls on a simple
class.
