# Shaft Programming Language Syntax Specification

## Contents
1. [Variables & Memory Model](#1-variables--memory-model)
2. [Data Structures & Readability](#2-data-structures--readability)
3. [Control Flow](#3-control-flow)
4. [Functions & Tunnels](#4-functions--tunnels)
5. [Keywords List](#5-keywords-list)
6. [Operators List](#6-operators-list)
7. [Safe Macros](#7-safe-macros)

> **Note:** Shaft uses **RAII** (Resource Acquisition Is Initialization) for resource cleanup.

---

## 1. Variables & Memory Model

### Primitive Types
```Shaft
// Signed and unsigned integer types
u8   i8
u16  i16
u32  i32
u64  i64

// Floating-point types
f32  f64

// Pointers, references, and mutability
T* // Raw pointer
&T       // Read-only reference (multiple allowed)
&mut T   // Writer reference (strict single writer xor multiple readers rule)
mut T    // Mutable variable declaration
?T       // May have a 1-byte validity tracker

// Arrays
T[]      // Dynamically-sized array
T[N]     // Fixed-size array of length N

// Standard library types
string   // Length + u8* data (guaranteed UTF-8, owning)
str      // u8[] non-growable string slice reference (non-owning)
cstr     // u8* data + '\0' (C-compatible string)
```

### Borrowing & Ownership
```Shaft
// Copy data into var1
var1 = expr;

// Ownership transfer: var1 gains ownership, var2 becomes invalid
var1 = move var2;

// Immutable reference binding: var1 gets reference, var2 becomes read-only
var1 = ref var2;
```

### Validity Check
To handle branched or conditional invalidation, validity can be queried explicitly. If static compiler analysis cannot guarantee validity at compile time, it has to be a ?T and a 1-byte validity flag is attached to the variable at runtime. If it cannot guaranteed be valid and is not a ?T, that is a compile-time error.

```Shaft
valid IDENT
{
    // Executed if IDENT is valid
}
else
{
    // Executed if IDENT has been invalidated or moved
}
```
### Raw strings
```
raw -> My_Custom_Delimitter
Here could be some code, or
everything else. Everything gets
baked into the string until I write:
My_Custom_Delimitter
```
---

## 2. Data Structures & Readability

```Shaft
// Namespaces
namespace foo
{
    def bar() {}
}

foo::bar();

// Enumerations
enum Fruit : u8
{
    BANANA,
    APPLE
}

Fruit my_fruit = Fruit::BANANA;

// Structures
struct Vector2
{
    f32 x;
    f32 y;
}

// Generic Structures
struct<T> some_struct
{
    T var;
}

// Struct Instantiation & Mutability
mut Vector2 point = {0, 0};
point.x = 5;
```

---

## 3. Control Flow

```Shaft
// Conditional branching
if (cond)
{
}
else
{
}

// Pattern matching
match (var)
{
    case val1
    {
    }
    case val2
    {
    }
    default
    {
    }
}

// Loops (support `break` and `continue`)
while (cond)
{
}

for (stmt; cond; stmt)
{
}

foreach (type name : container)
{
}
```

---

## 4. Functions & Tunnels

* Functions use **tunnels** (`tunnel expr -> slot;`) to return values.
* Tunnels **do not end function execution** automatically; multiple tunnels can be executed before exiting.
* Prefix modifiers include `inline`, `extern`, `export`, `cdef` (defines C-ABI function), and `cdec` (declares C-ABI function).
* Use `import "path"` to load external modules.

```Shaft
// Function Declaration
// Guaranteed return slot: `foo` of type `T`
// Optional return slot: `bar` of type `i32` (denoted by `?->`)
dec name(params)<T> -> T foo, ?-> i32 bar;

// Function Definition
def name(params)<T> -> T foo, ?-> i32 bar
{
    tunnel expr -> T foo; // Assign value to required return slot

    if (cond)
    {
        tunnel 5 -> i32 bar; // Conditionally populate optional return slot
    }
}
```

---

## 5. Keywords List

| Category | Keywords |
| :--- | :--- |
| **Control Flow** | `if`, `else`, `match`, `case`, `while`, `for`, `foreach` |
| **Declarations** | `def`, `dec`, `cdef`, `cdec`, `struct`, `namespace`, `using`, `import`, `export` |
| **Functions** | `tunnel`, `inline` |
| **Memory & Types** | `mut`, `move`, `ref`, `valid`, `raw` |
| **Primitive Types** | `u8`, `u16`, `u32`, `u64`, `i8`, `i16`, `i32`, `i64`, `f32`, `f64` |

---

## 6. Operators List

```
Assignment / Arithmetic :  =   +=   -=   *=   /=   %=
Bitwise / Shift         :  &   |    ^    <<   >>   <<=  >>=
Comparison              :  <   >    <=   >=
Access / Scope          :  ::  .    ->
Grouping / Delimiters   :  ( ) [ ]  { }  :    ;    ,
Other                   :  ... ?    "    '    +    -    * /    %
```

---

## 7. Safe Macros

Shaft replaces preprocessor macros with scoped, type-safe token alias declarations using the `using` syntax:

```Shaft
using target_alias -> replacement_type_or_expr;

// Example:
using int64 -> i64;
```
or for strings (but still does not cut tokens in half, must end with a '!'):
```Shaft
using "my_macro!" -> raw MACRO_END
def add(i32 x, i32 y) -> i32 result
{
    tunnel x + y -> i32 result;
}
```
