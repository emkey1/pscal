# C-Like Language Specification

### **Introduction and Design Philosophy**

This document specifies the C-like language, a front end for the PSCAL virtual machine (VM). The language is designed to offer a familiar, C-style syntax for developers, while compiling down to the same bytecode as the Pascal front end. This allows for a choice of syntactic flavors while targeting a single, unified runtime environment.

The language is strongly typed and supports a range of features common to C and other structured programming languages, including pointers, structs, and a rich set of control flow statements.

### **Lexical Structure**

#### **Comments**

* **Single-line comments:** Start with `//` and extend to the end of the line.
* **Multi-line comments:** Start with `/*` and end with `*/`. These cannot be nested.

#### **Keywords**

The following are reserved keywords and cannot be used as identifiers:

* `int`, `long`, `void`, `float`, `double`
* the combinations `long double`, `long long`
* `str`, `text`, `mstream`, `char`, `byte`
* `if`, `else`, `while`, `for`, `do`, `switch`, `case`, `default`
* `struct`, `enum`, `const`
* `break`, `continue`, `return`
* `import`

#### **Identifiers**

Identifiers are used for the names of variables, functions, and user-defined types. They must begin with a letter or an underscore (`_`) and can be followed by any number of letters, digits, or underscores.

#### **Literals**

* **Integer Literals:** Can be decimal (e.g., `123`) or hexadecimal (e.g., `0x7B`).
  Unsuffixed integer literals are interpreted as 64-bit signed values.
* **Floating-Point Literals:** Written with a decimal point (e.g., `3.14`).
* **Character Literals:** Enclosed in single quotes (e.g., `'a'`). They support standard C-style escape sequences (`\n`, `\r`, `\t`, `\\`, `\'`).
* **String Literals:** Enclosed in double quotes (e.g., `"hello"`). They also support C-style escape sequences.

### **Data Types**

The language supports a variety of built-in data types:

| Keyword | VM Type | Description |
| :--- | :--- | :--- |
| `int` | `TYPE_INT32` | 32-bit signed integer. |
| `long`, `long long` | `TYPE_INT64` | 64-bit signed integer. |
| `float` | `TYPE_FLOAT` | 32-bit floating-point number. |
| `double` | `TYPE_DOUBLE` | 64-bit floating-point number. |
| `long double` | `TYPE_LONG_DOUBLE` | Extended precision floating-point number. |
| `char` | `TYPE_CHAR` | Unicode code point. |
| `byte` | `TYPE_BYTE` | 8-bit unsigned integer. |
| `str` | `TYPE_STRING` | Dynamic-length string. |
| `text` | `TYPE_FILE` | File handle for text I/O. |
| `mstream` | `TYPE_MEMORYSTREAM` | In-memory byte stream. |
| `void` | `TYPE_VOID` | Absence of value (used for procedures). |

### **Variables and Constants**

#### **Variable Declarations**

Variables must be declared with their type before they are used. The syntax is:

```c
type identifier;
type identifier = initial_value;
```

**Example:**

```c
int x;
float y = 3.14;
str name = "world";
```

#### **Constant Declarations**

Constants can be declared using the `const` keyword. Their values must be known at compile time.

```c
const int MAX_SIZE = 100;
```

### **Expressions and Operators**

The language supports a standard set of operators with C-like precedence.

| Precedence | Operator | Associativity |
| :--- | :--- | :--- |
| 1 | `()` `[]` `.` `->` `++` `--` | Left-to-right |
| 2 | `!` `~` `-` `+` `*` `&` | Right-to-left |
| 3 | `*` `/` `%` | Left-to-right |
| 4 | `+` `-` | Left-to-right |
| 5 | `<<` `>>` | Left-to-right |
| 6 | `<` `<=` `>` `>=` | Left-to-right |
| 7 | `==` `!=` | Left-to-right |
| 8 | `&` | Left-to-right |
| 9 | `^` | Left-to-right |
| 10 | `|` | Left-to-right |
| 11 | `&&` | Left-to-right |
| 12 | `||` | Left-to-right |
| 13 | `?:` | Right-to-left |
| 14 | `=` `+=` `-=` `*=` `/=` `%=` `&=` `|=` `<<=` `>>=` | Right-to-left |

### **Statements**

* **Expression Statements:** Any valid expression followed by a semicolon is a statement.
* **Compound Statements (Blocks):** A sequence of statements enclosed in curly braces (`{}`).
* **`if` Statements:**
    ```c
    if (condition) {
      // ...
    } else {
      // ...
    }
    ```
* **`while` Loops:**
    ```c
    while (condition) {
      // ...
    }
    ```
* **`for` Loops:**
    ```c
    for (initialization; condition; increment) {
      // ...
    }
    ```
* **`do-while` Loops:**
    ```c
    do {
      // ...
    } while (condition);
    ```
* **`switch` Statements:**
    ```c
    switch (expression) {
      case constant_expression:
        // ...
        break;
      default:
        // ...
    }
    ```
* **`break` and `continue`:** Used to alter the flow of loops and `switch` statements.
* **`return`:** Returns a value from a function.

### **Functions**

Functions are declared with a return type, a name, and a list of parameters.

```c
return_type function_name(parameter_list) {
  // function body
}
```

**Example:**

```c
int add(int a, int b) {
  return a + b;
}
```

### **Structs**

Structs are user-defined data types that group together variables of different data types.

```c
struct MyStruct {
  int x;
  float y;
};
```

You can then declare variables of this new type:

```c
struct MyStruct s;
s.x = 10;
```

### **Pointers**

The language supports pointers using the `*` syntax.

* **Address-of operator (`&`):** Returns the memory address of a variable.
* **Dereference operator (`*`):** Accesses the value at a memory address.

**Example:**

```c
int x = 10;
int* p = &x;
*p = 20; // x is now 20
```

### **Preprocessor Directives**

* **`#import`:** Includes another source file.
* **`#ifdef`, `#ifndef`, `#else`, `#elif`, `#endif`:** For conditional compilation based on whether a symbol is defined.

### **Threading and Synchronization**

The language provides lightweight concurrency and mutex primitives through the following built-ins:

* `spawn` – starts a new thread executing a parameterless function and pushes its integer thread identifier.
* `join` – waits for the thread with the given identifier to finish execution.
* `mutex` – creates a standard mutex and pushes its integer identifier.
* `rcmutex` – creates a recursive mutex and pushes its identifier.
* `lock` – pops a mutex identifier and blocks until it is acquired.
* `unlock` – releases the mutex whose identifier is on top of the stack.
* `destroy` – pops a mutex identifier and permanently frees the mutex.

Example:

```c
int tid = spawn worker();
join tid;
```

### **Built-in Functions**

The C-like front end has access to the rich set of built-in functions provided by the PSCAL VM, including file I/O, string manipulation, mathematical functions, and more. Some common C library functions are also mapped to their Pascal equivalents (e.g., `strlen` is mapped to `length`).

### **Example Code**

Here is a simple "Hello, World!" program to demonstrate the language's syntax:

```c
int main() {
  printf("Hello, World!\n");
  return 0;
}
```

 This example uses the `printf` built-in function to print a string to the console.
