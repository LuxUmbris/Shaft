# Shaft language specification

## Lexical structure

### Whitespace, comments, and identifiers

Whitespace separates tokens. Line comments begin with `//`; block comments use `/* ... */`; documentation comments use `///`. All three comment forms are ignored by the compiler. The current native `shaftls` server does not derive hover content or compiler diagnostics from documentation comments.

Identifiers are distinct from keywords. Qualified identifiers use `::`:

```shaft
Collections::HashMap
Core::answer
```

The lexer recognizes the keywords listed in the compiler header, including `def`, `dec`, `cdef`, `cdec`, `struct`, `enum`, `union`, `trait`, `impl`, `namespace`, `using`, `global`, `import`, `export`, `if`, `while`, `for`, `foreach`, `do`, `yield`, `return`, `tunnel`, `reserve`, `move`, `ref`, and the primitive type names. `class` is rejected; data uses a struct, enum, or union and behavior is attached with `impl`.

### Literals

Literal categories are unsigned integer, floating-point, boolean, character, and string literals.

```shaft
u64 count = 42;
f64 ratio = 1.5;
bool ready = true;
char marker = 'A';
String text = "hello";
```

Integer literals infer as `u64`; floating-point literals infer as `f64`. Numeric assignment and call checking permits conversion among numeric primitive types. The backend casts at the receiving operation, assignment, call, return, or tunnel boundary.

String literals are lowered through the bundled `String`/`str` prelude ABI. Escape decoding recognizes `\n`, `\r`, `\t`, `\0`, `\\`, `\"`, `\'`, two-digit byte escapes (`\xNN`), and four-digit Unicode scalar escapes (`\uNNNN`), which lower to UTF-8. Invalid hexadecimal, incomplete, and surrogate Unicode escapes are compilation errors.

The lexer also implements delimiter-terminated raw strings:

```shaft
raw -> END
any source text, including quotes
END
```

The delimiter is an identifier placed alone at the start of its closing line. Raw-string lexing is implemented; use it only where the surrounding parser accepts a string literal.

### Operators and punctuation

The lexer uses longest-match tokenization for multi-character operators. Operators include:

```text
=  +=  -=  *=  /=  %=  <<=  >>=
||  &&  |  ^  &
==  !=  <  <=  >  >=  <<  >>
+  -  *  /  %
!  &  *  .  ::  ()  []  {}  ,  ;  ->  <-  ?
```

## Types

### Primitive types

The accepted primitive type names are:

```text
bool  char
u8 u16 u32 u64 usize
i8 i16 i32 i64
f32 f64
State Thread
```

Backend representation in this revision:

| Shaft type | LLVM representation |
| --- | --- |
| `bool` | `i1` |
| `u8`, `i8` | `i8` |
| `u16`, `i16` | `i16` |
| `u32`, `i32`, `char` | `i32` |
| `u64`, `i64`, `usize` | `i64` |
| `f32` | `float` |
| `f64` | `double` |
| enum | declared integral backing type; `i32` when omitted |

`usize` is lowered to an unsigned integer matching the selected target's native pointer width. `State` is an executable cooperative deferred-call handle. `Thread` is distinct: native thread backends are available for x86_64, AArch64, and RISC-V Linux, and x86_64/AArch64 Darwin/macOS and Windows, as described in §11.

### Compound and named types

The grammar accepted for types is:

```text
type           ::= type-prefix* type-base array-suffix*
type-prefix    ::= "mut" | "*" ["mut"] | "&" ["mut"] | "?"
type-base      ::= primitive-type | qualified-name ["<" type ("," type)* ">"] | "(" type ")"
array-suffix   ::= "[" [integer-literal | identifier] "]"
qualified-name ::= identifier ("::" identifier)*
```

Examples:

```shaft
* i8 rawBytes;
&mut u64 writer;
&(u8[64]) wholeBuffer;
mut (mut u8)[64] writableBytes;
?String maybeName;
u8[16] fixed;
String[] argv;
Collections::HashMap<String, u64> counts;
```

A parenthesized type is transparent grouping, but it controls which type an array suffix or prefix wraps: `&(u8[64])` is a reference to one 64-byte array, while `mut (mut u8)[64]` is a mutable fixed array whose `u8` members are mutable. A fixed-size array `T[N]` lowers to an LLVM array. A named runtime array `T[name]` requires a previously declared `u64` or `usize` local length binding; the allocation snapshots that count, and named runtime arrays cannot be struct fields. `T[]` is the borrowed pointer-style form used where a separate count is supplied by the surrounding ABI. Pointer and reference types both lower as opaque LLVM pointers. References carry checker-enforced lexical aliasing: one `&mut T` writer is exclusive, while any number of `&T` readers may coexist; readers and a writer may not coexist.

Qualified custom type identity is canonical: `Left::Token` and `Right::Token` are distinct types.

Optional values lower to a tagged LLVM aggregate containing a presence flag and a payload. `?T value = expression;` creates a present value, while an uninitialized `?T value;` is absent. `valid value { ... } else { ... }` branches on that flag; value reads in the valid branch use the payload. Optional copy preserves both flag and payload.

### Generic syntax

Structs, classes, and Shaft `def` definitions accept simple name-only generic parameter lists. `dec` declarations cannot be generic because generic functions require a Shaft definition for specialization. Named types and explicit generic calls accept type arguments. Nested closing `>>` is accepted as two generic closers.

```shaft
struct<T, U> Pair { T first; U second; }
def identity(T value)<T> -> T result { tunnel value -> T result; }
identity::<u64>(42);
```

The bootstrap checker resolves generic field types through each concrete named-type instantiation. The backend specializes generic structs/classes on their type arguments and emits distinct concrete LLVM layouts. Explicit generic calls (`function::<T>(...)`) specialize Shaft `def` functions into distinct concrete LLVM symbols. `Vector<T>` retains its dedicated bootstrap representation.

## 4. Declarations, scopes, and storage

### Variables

```text
variable-declaration ::= type identifier ["=" initializer] ";"
initializer           ::= expression | "move" identifier | "ref" identifier
```

Examples:

```shaft
i32 value = 40;
mut u64 total = 0;
String label = "shaft";
```

Bindings are immutable by default: direct reassignment requires `mut`, and member/index mutation requires mutable root storage plus `mut` on every traversed and target field. Shaft has no `static` storage declaration, so mutable static state cannot be declared. Variables are lexically scoped by blocks and namespaces. Redeclaring a variable in the same scope is an error.

### Expressions and precedence

Postfix operations bind most tightly, then unary operators, then binary operators, then right-associative assignment.

```text
postfix       ::= primary { "(" arguments? ")" | "[" expression "]"
                         | "." identifier | "::" identifier
                         | "::" "<" type-list ">" "(" arguments? ")" }
unary         ::= ("!" | "-" | "+" | "*" | "&" | "await") unary | postfix
multiplicative ::= unary { ("*" | "/" | "%") unary }
additive      ::= multiplicative { ("+" | "-") multiplicative }
shift         ::= additive { ("<<" | ">>") additive }
comparison    ::= shift { ("<" | "<=" | ">" | ">=" | "==" | "!=") shift }
bitwise-and   ::= comparison { "&" comparison }
bitwise-xor   ::= bitwise-and { "^" bitwise-and }
bitwise-or    ::= bitwise-xor { "|" bitwise-xor }
logical-and   ::= bitwise-or { "&&" bitwise-or }
logical-or    ::= logical-and { "||" logical-and }
assignment    ::= logical-or [assignment-operator assignment]
expression    ::= assignment
```

Assignment operators are `=`, `+=`, `-=`, `*=`, `/=`, `%=`, `<<=`, and `>>=`. Assignment is right-associative; binary operators are left-associative.

The checker validates numeric arithmetic, Boolean `&&`/`||`, comparisons, pointer-plus/minus-numeric forms, and compatible assignment/call types. The LLVM backend implements scalar arithmetic, comparisons, shifts, assignments, calls, member access, indexing, and the tested pointer forms; expressions lacking a lowering rule fail compilation rather than silently changing semantics.

### `do` expressions

`do` introduces a lexical runtime scope in an expression position. It executes ordinary statements and must end with `yield expression;`; the yielded value is the expression result.

```shaft
reserve i32 answer = do {
    reserve i32 offset = 2 as i32;
    yield 40 + offset;
};
```

For an owned result created inside the scope, use `yield move value;` so scope cleanup is transferred to the caller. `do` is ordinary runtime syntax, not a host-executing comptime escape hatch. It provides the expression-scoped storage needed by source-generated facilities such as future owned formatting expressions.

### Blocks and statements

A block is `{ statement* }`. Expression statements and variable declarations require `;`.

```shaft
{
    i32 value = 1;
    value += 2;
    consume(value);
}
```

The parser requires braces for `if`, `else`, loop bodies, function bodies, namespace bodies, and match arms. There is no single-statement unbraced body form.

## Functions and ABI

### Shaft functions: `def` and `dec`

Shaft functions use output tunnels rather than a C return value:

```text
definition ::= "def" ["async"] identifier "(" parameters? ")"
               ["<" generic-parameters ">"] tunnel-slots block
declaration ::= "dec" ["async"] identifier "(" parameters? ")"
               tunnel-slots ";"
tunnel-slots ::= [ ["?"] "->" type identifier ]
```

```shaft
def add(i32 lhs, i32 rhs) -> i32 result {
    tunnel lhs + rhs -> i32 result;
}
```

A non-optional tunnel slot must be populated on every checked path. The checker rejects missing required slots, unknown slot names, type mismatches, and a slot that may be written more than once. `tunnel value -> T slot;` must restate the slot type syntactically; the checker compares it with the declared slot type. `async def` and `async dec` use the same native tunnel ABI as `def`/`dec`; they can be scheduled cooperatively through `State`. A native `Thread` entry must instead be a non-generic void `def` without tunnel outputs.

`def` lowers as an LLVM `void` function with tunnel results passed as output pointers. Optional results additionally receive native presence-flag output pointers; an unfilled result produces an absent optional, while a tunneled value produces a present optional with its payload. A single tunnel may be used as a call expression. Bind multiple required outputs exactly once with `reserve T first, U second = call();`; each binding receives its corresponding slot in declaration order. A direct `__shaft_alloc` or `__shaft_alloc_or_exit` result, or an aggregate containing compiler-owned allocation storage, cannot be tunneled because tunnel payloads do not yet transport ownership metadata.

`return` is invalid in `def` and `dec`; use `tunnel`.

### Unsafe and no-check regions

```text
unsafe-region  ::= "@unsafe" statement* "@end"
nocheck-region ::= "@nocheck" statement* "@end"
```

Raw pointer types, `&` address formation, `*` dereferencing, inline assembly, C-ABI declarations/definitions, and C-ABI calls must be lexically enclosed by `@unsafe ... @end`. `@nocheck ... @end` is a nested scoped directive for opting out of compiler-inserted dynamic checks; it does not authorize unsafe operations and does not weaken the `@unsafe` requirement.

```shaft
@unsafe
cdec memcpy(*i8 destination, *i8 source, u64 bytes) -> *i8;
@end
```

### C-ABI functions: `cdef` and `cdec`

```text
c-definition ::= "cdef" identifier "(" parameters? ")" ["->" type] block
c-declaration ::= "cdec" identifier "(" parameters? ")" ["->" type] ";"
parameters    ::= parameter ("," parameter)*
parameter     ::= type identifier
```

```shaft
@unsafe
cdef add(i32 lhs, i32 rhs) -> i32 {
    return lhs + rhs;
}

cdec memcpy(*i8 destination, *i8 source, u64 bytes) -> *i8;
@end
```

`return` is valid only in `cdef`. A value-returning `cdef` must return a compatible value; a void `cdef` must not return one. C declarations are predeclared in code generation, allowing calls before their definitions. Function symbols are predeclared by the checker, allowing ordinary calls before a later definition.

### Hosted C libraries

`cdec` declarations bind unmangled C symbols. Normal Shaft binaries are freestanding and deliberately do not link libc; use `--hosted` for a host-native binary that calls a C library. `--link NAME`, `-l NAME`, and `-lNAME` each forward `-lNAME` to the selected Clang/LLD toolchain, using the same system library search paths as a normal host link. `--link-dir PATH` adds a library search path. Library options require `--hosted`, which is intentionally unavailable for cross-target binaries.

Raw `.o`, `.a`, `.ll`, `.llvm`, and `.bc` files may be listed alongside the one `.shaft` input when emitting a binary or dynamic library. Shaft preserves their order relative to `-l` arguments, so place a static archive after the object or library that needs its symbols:

```text
shaftc --no-std --hosted main.shaft bridge.o libhelpers.a extension.bc -lraylib
```

For a no-standard-library program whose entry is `cdef main`, Shaft generates a temporary host `main` bridge. With the standard prelude enabled, the Linux runtime instead supplies that bridge and preserves the regular `def main(String[] args)` contract.

```shaft
@unsafe
cdec InitWindow(i32 width, i32 height, *i8 title);
cdec WaitTime(f64 seconds);
cdec CloseWindow();

cdef main() -> i32 {
    InitWindow(640, 360, "Shaft + raylib");
    WaitTime(1.0);
    CloseWindow();
    return 0;
}
@end
```

Build it against a host-installed raylib:

```text
shaftc --no-std --hosted -lraylib raylib-window.shaft
```

Use `--link-dir /path/to/lib` when raylib is outside the system linker paths. C declarations must match the library's C ABI exactly; model C `bool` as `i32` unless the target API documents another integer width.

`def main` is emitted as `__main`. The bundled freestanding entry bridge converts process arguments to `String[]`, calls `__main`, exits with `0` on normal completion, and honors `exit(N)` for a nonzero process status. This bridge requires the standard library/runtime resources; it is not enabled by `--no-std` alone.

### Methods and `self`

Struct, enum, and union declarations contain only data. Methods live in an `impl` block. A trait declares method signatures, and `impl Trait for Type` attaches matching behavior to a struct, enum, or union. Inherent behavior uses `impl Type`. A first method parameter may be:

```shaft
self
&self
&mut self
```

The parser resolves `Self` relative to the impl target. The backend lowers method names using the target type and namespace name and supports direct receiver method calls for addressable receivers. Shaft has no class inheritance; choose a struct, enum, or union representation and compose behavior with traits and impls.

## Control flow

### Executable control flow

```shaft
if (condition) { ... } else { ... }
while (condition) { ... }
break;
continue;
```

`else if` is parsed as an `else` containing another `if`. `break` and `continue` are checker errors outside a loop. `if` and `while`, including `break` and `continue`, are lowered by the backend.

Conditions require Boolean values. `&&` and `||` are lowered with control-flow short-circuiting: their right-hand side executes only when the left-hand side does not determine the result.

### Selection and iteration

```shaft
match (subject) {
    case value { ... }
    default { ... }
}

for (initializer; condition; post) { ... }
foreach (T item : fixedArray) { ... }
```

`match` evaluates its subject once, compares each case in declaration order, and executes at most one matching arm. `default`, when present, must be the final arm. `for` supports optional initializer, condition, and post clauses; initializer bindings are scoped to the loop, and `continue` executes the post clause before re-checking the condition. `foreach` iterates fixed-size `T[N]` arrays and named runtime arrays `T[length]` in ascending index order, binding an immutable item alias for each body execution. A runtime-array length binding must be declared before its array.

`valid name { ... } else { ... }` requires a declared `?T` local and lowers to a presence-flag branch. The `else` block is optional.

## User-defined data types

### Structs, traits, and implementations

```text
struct-declaration ::= "struct" ["<" generic-parameters ">"] identifier struct-body
trait-declaration  ::= "trait" identifier "{" method-declaration* "}"
impl-declaration   ::= "impl" ["<" generic-parameters ">"] [trait-name "for"] type "{" method* "}"
aligned-struct     ::= "align" integer-literal "struct" ["<" generic-parameters ">"] identifier struct-body
```

```shaft
struct Point {
    i32 x;
    i32 y;
}

struct Buffer {
    *u8 data;
    u64 length;
}

trait Sized { dec len(&self) -> u64 result; }
impl Sized for Buffer { def len(&self) -> u64 result { tunnel self.length -> u64 result; } }
```

Fields are collected from a struct body; methods are collected from impl blocks. A field assignment is mutable only when both its root struct binding and every traversed field are declared `mut`:

```shaft
struct Cell { mut i32 value; }
struct Grid { mut Cell[2] cells; }

mut Grid grid;
grid.cells[0].value = 42;
```

The checker resolves this recursively through members and arrays. Removing `mut` from `grid`, `cells`, or `value` makes that assignment a checker error. `align N struct` requires a nonzero power-of-two `N`; the backend preserves it and applies it to local LLVM allocations of that struct type.

Struct `@index field;` declares the direct backing field used by `receiver[index]`. It is valid only once and only when `field` is a pointer or runtime-sized array. `@init field;` is also unique and must name the same field as `@index`. The backend lowers both index access and brace construction for an `@init`-configured struct by allocating its backing storage through `__shaft_alloc`.

```shaft
mut Vector<i64> values = { 40, 0 };
```

Plain positional struct literals use the same surface syntax and initialize declared fields in source order. The initializer must supply exactly one value for every field, with each value convertible to its corresponding field type. Structs that declare an `@init` backing field retain their allocation-backed brace-construction behavior.

### Enums

```shaft
enum Color : u32 {
    Red,
    Green = 2,
    Blue,
}
```

Enums lower using their declared integral backing type (or signed `i32` when omitted). Member values accept integer constant expressions using earlier enum members and integer arithmetic/bitwise operators; omitted values continue from the preceding member. Qualified member uses produce constants of that backing type.

## Namespaces, export, import, and using macros

### Namespaces

```shaft
namespace collections {
    struct HashMap<K, V> { ... }
}
```

Namespaces create checker/code-generation qualification contexts. Nested names use `::` in source type identity. The current backend uses dot-separated LLVM symbol components for namespace/type method names. This is an implementation ABI detail, not source syntax.

### Using namespaces globally

```shaft
using foo::bar;
using foo::bar::baz;
using namespace foo;
```

### Export and import

```shaft
export cdef api(i32 value) -> i32 { return value; }
import "module.shaft";
```

`export` is an executable transparent declaration wrapper: the backend lowers its enclosed declaration normally and gives a wrapped `def` external LLVM linkage. Ordinary Shaft `def` declarations use private LLVM linkage; the hosted `main` bridge remains externally visible for the runtime. `import "module.shaft";` loads a Shaft source module relative to the importing file. Imports are compile-time dependency directives: each canonical file is loaded once, cycles are rejected, and dependencies compile before their importers. Imported declarations share the project-wide checker/code-generation namespace.

### `using` macros

```shaft
@using alias! -> replacement;
@using "name!" -> replacement;
@using call!($expr:left, $expr:right) -> $left + $right;
@using global public_call!($tt:values...) -> target($values);
```

An `@using` macro is a scoped lexer expansion. Macro declarations visibly use `@using`; ordinary `using` remains reserved for namespace imports. Every identifier macro invocation requires `!`: object-like macros use `alias!`, while function-like macros use `call!(arguments...)`. Typed parameters use `$fragment:name`: `$expr`, `$ty`, `$ident`, `$literal`, `$int`, `$float`, `$str`, `$char`, `$bool`, or `$tt`. `$literal` accepts exactly one literal token, while `$int`, `$float`, `$str`, `$char`, and `$bool` require their respective literal token kinds. Function-like macros may overload the same alias and arity when their fragment signatures differ; overload selection is syntax-directed and selects the first matching fragment signature. Final `$tt:name...` captures any number of comma-separated token trees. In a replacement, write `$name` to splice a captured token fragment (including comma-separated splicing for `$tt:name...`); bare identifiers are retained for compatibility but `$name` avoids accidental capture and is the canonical token-manipulation spelling. A `$name` not captured by the surrounding macro is preserved for a nested `@comptime` token program to bind, enabling source macros to pass captured literals into comptime loops. Replacements may be balanced blocks and can invoke macros declared earlier. Direct recursion is rejected except for structural recursion that consumes a final `$tt:name...` sequence.

`@using` is private to its source module and lexical scope. `@using global` exports an alias to later compiler source modules. The standard prelude uses global macros for public output convenience APIs. Local declarations shadow global ones. Macro expansion is hygienic for supported local declaration forms. Lexer/parser diagnostics that arise from generated tokens include an `expanded from @using macro ... invoked at file:line:column` note; nested expansions retain their earlier notes. Checker diagnostics from generated macro calls use the invocation as their primary span. Expansion is declaration-order-sensitive, and generated replacement tokens are bounded by a per-replacement 16,384-token limit and a global 1,000,000-token ceiling.

### Procedural derives

```shaft
@procedural derive marker($struct:target)
    @quote
        def marker_for($target value) -> i32 result { tunnel 42 -> i32 result; }
    @end
@end

@derive marker
struct Foo { i32 value; }
```

`@procedural derive` is distinct from `@using`: it attaches one or more comma-separated named procedures to the immediately following struct declaration and emits each `@quote` after that declaration, in `@derive` order. Each procedure receives the attached struct's nominal type and substitutes `$target` in ordinary quoted Shaft declarations. The emitted declarations then pass through the normal lexer, parser, checker, and LLVM lowering pipeline. The struct header and its opening brace may use either Allman or K&R layout.

### Procedural attributes (declaration slice)

```shaft
@procedural attribute counted($ident:prefix) for $struct:target
    @quote
        u64 $(prefix)_$(target.name)_fields = $target.field_count;
    @end
@end

@counted(zz)
struct Foo { i32 value; i32 other; }
```

`@procedural attribute` is distinct from both `@using` and `@procedural derive`: it defines a named attribute that is attached to the declaration that immediately follows a `@name(arguments)` use site. Unlike a derive, the attribute is expanded in the parser against the real declaration node, so its template can inspect the target (its name, parameters, fields, enum members, union variants, or type) and take attribute arguments. The header names the attribute, its parameters as `$kind:capture`, and the attached declaration kind as `for $kind:target`. The emitted declarations are lexed and parsed like ordinary source and then pass through the normal checker and LLVM lowering pipeline. Errors in generated code are reported against `<expansion of @name at path>`.

```shaft
@procedural attribute traced($string:label) for $function:target
    @prepend
        trace_enter("$label", $target.param_count);
    @end
    @quote
        @for $p in $target.params
            // one line per parameter: $p.name has type $p.type
        @end
    @end
@end

@traced("move")
def move(i32 dx, i32 dy) -> i32 moved { tunnel dx + dy -> i32 moved; }
```

**Definition.** The body of an attribute is one or more sections, each closed by its own `@end`, followed by a final `@end` for the attribute.

- `@quote` holds generated declarations. They are emitted after the attached declaration by default. A line containing only `@original` marks the position of the attached declaration, so declarations before it are emitted ahead of the target and declarations after it follow.
- `@prepend` holds statements that are inserted at the start of the attached function's body. It is valid only for `$function` targets that have a body.
- Parameter kinds are `string`, `int`, `ident`, and `bool`. The use site must pass exactly one literal of that kind per parameter. Negative integers are not literals and are rejected.
- Target kinds are `function` (`def`, `dec`, `cdef`, `cdec`), `struct`, `union`, `enum`, and `variable`. Attaching an attribute to a different declaration kind is an error at the use site.
- An attribute may not reuse the name of an existing directive or keyword (`@comptime`, `@using`, `@end`, `@index`, `@init`, `@derive*`, and similar), and may not be defined twice.

**Use sites.** `@name(args)` appears immediately before a declaration, optionally before `export`-wrapped declarations. Several attributes may be stacked. The attribute closest to the declaration is applied first. An attribute with no parameters may omit its parentheses. Attributes are accepted at module level, inside namespaces, and inside struct bodies.

**Templates.** Quoted text is ordinary Shaft with the following substitutions, all resolved before the text is parsed.

- `$capture` inserts an attribute argument. `$target` and `$target.name` insert the attached declaration's name. `$(path)` is the delimited form, for example `$(target.name)_count`. `$$` inserts a literal `$`.
- `$target.kind` and `$target.exported` are available for every target.
- Functions: `$target.params` and `$target.slots` (lists), `$target.param_count`, `$target.slot_count`, `$target.ret` (the `cdef`/`cdec` return type, empty otherwise), `$target.has_body`, and `$target.is_async`. Parameters expose `name` and `type`. Tunnel slots expose `name`, `type`, and `optional`.
- Structs: `$target.fields` and `$target.field_count`. Fields expose `name`, `type`, and `has_default`.
- Enums: `$target.members`, `$target.member_count`, and `$target.backing`. Members expose `name`, `has_value`, and `value` (set when the initializer is an integer literal).
- Unions: `$target.variants` and `$target.variant_count`. Variants expose `name`, `payload`, and `payload_count`.
- Variables: `$target.type` and `$target.has_init`.
- `@for $item in $target.list` ... `@end` repeats its body once per element, exposing `$item.name`, `$item.type`, and so on. Every item also provides `$item.index`, `$item.comma` (`", "` except on the last item), and `$item.last`.
- `@if $flag` ... `@end` and `@if !$flag` ... `@end` include their body conditionally. A value is false when it is empty, `0`, or `false`.
- `@reject message` stops compilation with an error at the use site when it is reached, which makes it the validation hook: `@if !$target.has_body` followed by `@reject $target.name needs a body` and `@end`.

Types are reflected as source text. Pointer, reference, optional, array, generic, and qualified custom types round-trip, and `typeof` types are rejected with a diagnostic.

This is intentionally **not yet** the full semantic-AST system: attributes cannot rewrite or remove the attached declaration, only emit declarations around it and prepend statements to a function body. Generic parameters, struct methods, existing attributes, `has_attr`, and expression bodies are not reflected. Attributes are not accepted on local statements or on `impl` and `trait` members, and they have no procedural host execution or shell access. Attribute names are global, so an attribute must be defined in the same module as its use or in a module lexed earlier. Expansion is bounded, and attribute expansion nested deeper than 32 levels is an error. Those forms remain unavailable rather than being silently treated as token macros.

The existing `@comptime` forms can still appear in generated normal Shaft code under their documented deterministic restrictions.

### Standard string conversion and formatting substrate

The standard prelude exposes `type_of!($expr:value)`, which expands to the checker-owned `typeof($value)` type query. It is valid wherever a type is expected and therefore preserves the resolved semantic type of a non-literal expression—for example, `mut type_of!(x + 2) result = 0;`. `to_string!($expr:value)` is an ordinary global macro built from a source-literal overload plus nested `@comptime iftype` expressions. It selects the standard `String::from_i*`, `from_u*`, `from_f*`, `from_bool`, `from_char`, `from_view`, or `from_str` conversion after checking the captured expression type; an existing owned `String` is returned directly. It supports source literals, C strings (`*u8`), and arbitrary non-literal scalar/string expressions. Unsupported types produce an ordinary unresolved-source diagnostic; no compiler formatter conversion is involved.

`format_into!(output, values...)` is a source-level block statement macro for an unbounded comma-separated sequence; invoke it without a following semicolon. It recursively emits a hygienic owned `String` temporary for each value through `to_string!`, appends its `get_view()` to the caller-owned output `String`, and preserves evaluation order. For example, `format_into!(output, "answer=", answer, " enabled=", enabled)` produces `answer=42 enabled=true` with no formatter-specific lowering. It currently concatenates supplied values rather than parsing `{}` placeholders.

The standard prelude exposes `format!("{}", values...)` and `println!("{}", values...)` as global variadic macros. A one-source-string-literal `format!("text")` is an ordinary source macro expanding to `std::string::String::from_str`, returning an owned `String`; the matching `println!("text")` expands to `std::io::println`. For a literal template plus values, `println!` is now also ordinary source expansion: it builds `Vector<String>` ownership storage, derives a `Vector<View>`, calls `std::string::format::render_many`, and prints the returned `String`. It accepts untyped `{}` placeholders and `{{`/`}}` escapes with any number of scalar, view, owned-string, or C-string arguments. This path has no formatter-name checker or codegen lowering.

The multi-placeholder expression form of `format!` remains interim: it still routes to the compatibility implementation while a general expression-producing source macro can create and return its owned `String` storage. `render_many` validates malformed braces and exact placeholder/value count at runtime and returns no value on failure; compile-time literal-template validation is not yet available from generic source metaprogramming.

### Standard string formatting substrate

`std::string::format::many(template, values, value_count)` formats a caller-owned contiguous sequence of `std::string::view::View` values. It accepts an arbitrary count, substitutes each unescaped `{}` in source order, treats `{{` and `}}` as literal braces, and returns `formatted = false` for malformed braces or a placeholder/value-count mismatch. On success it tunnels an owned `std::string::String`.

`many` is the runtime owned-string substrate beneath the macro formatter. It intentionally takes pre-converted views; `to_string!` now provides source-level semantic conversion for ordinary non-literal scalar, `View`, and owned-`String` expressions. The public multi-placeholder `format!` / `println!` route remains interim until generic comptime token accumulation can build the conversion storage and `View` sequence entirely in source.

### Configuration blocks and inline assembly

```shaft
@config.build.target = "x86_64-unknown-linux-gnu"
@asm
    nop
@end
@end
```

`@config.<build|package|format|meta>.<field> = value` starts a lexer macro block, closed by a matching `@end`. replacing the `=` with `matches` enables RegEx pattern matching, while `in` checks a json dictionary `[]`. The lexer compares the value exactly against the active compiler configuration. A matching block contributes its contents to the token stream; a nonmatching block is removed before imports, parsing, macro expansion, checking, and code generation. Nested `@config` blocks and `@asm` blocks are matched correctly. This makes target-specific source safe: code that is invalid for another target can live in a nonmatching block.

The supported scalar `build` fields are `entry`, `output`, `target`, `emit`, `optimization`, `stdlib`, `runtime`, `resources`, `no_std`, `native`, `hosted`, `check_only`, and `verbose`. The supported `package` fields are `name` and `version`, for `format` it is `brace_style`, `namespace_indent`, `match_case_indent`, `indent_width`, `use_tabs` and `final_newline`, while `meta` has `config_macros`, supplied by `Shaft.toml`; an unavailable field simply does not match. Array-valued build fields (`links` and `link_directories`) are intentionally not comparable. Strings may be quoted; booleans use `true` or `false`.

`@asm ... @end` is a function-body statement containing raw target assembly. It lowers to a side-effecting LLVM inline-assembly call. Use `@asm(value)` to pass an ordinary integer, pointer, reference, or enum binding as a read-only register operand; write `$value` in the assembly body to reference it. Use `@asm(mut value)` for an input/output register operand: it must name a mutable binding and its final register value is stored back into that binding. Multiple operands are comma-separated. Source `$` characters not naming a listed operand are preserved as literal target-assembly `$` characters, so ordinary AT&T immediates such as `$42` work directly. A `cdef naked name(...) -> T` definition has LLVM's `naked` attribute and its body may contain only `@asm` blocks; it emits no compiler prologue/epilogue, so its assembly must implement the target ABI and return itself (for example with `ret` on x86). The assembler syntax is the selected LLVM target's default (AT&T for x86); malformed or unsupported assembly is rejected by LLVM during native emission.

### Restricted `@comptime` functions

A build may also run a deliberately narrow, opt-in host command check:

```toml
# Shaft.toml
[comptime]
commands = ["/absolute/path/to/tool"]
```

```shaft
@comptime run "/absolute/path/to/tool" @end
```

The executable is canonicalized and must exactly match a regular executable explicitly allowlisted by `Shaft.toml`. The compiler invokes it directly with no shell, no arguments, an empty interactive stream, discarded output, a minimal environment, and a five-second timeout. This is Unix-host-only. It cannot read compiler output, generate tokens, receive arguments, or inherit the user's environment; broader host execution remains unsupported.

```shaft
@comptime
def answer() -> i32 result
{
    tunnel 40 + 2 -> i32 result;
}
@end
```

`@comptime ... @end` has four currently supported forms. Around a `def`, it declares a comptime-only function. Calls are lowered to a bounded stack-bytecode program during compilation: integer parameters are bound as bytecode locals; integer/bool literals, local reads, arithmetic, comparisons, local initialization/assignment, and `if`/`else` are executed; exactly one non-optional integer tunnel result is required. The call arguments must be integer constant expressions, and no LLVM declaration, definition, or call is emitted for the comptime helper. In expression position, `@comptime integer-expression @end` evaluates its body before LLVM lowering and materializes an integer constant. It is not a host execution facility.

Semantic type selection is available in expression position:

```shaft
@comptime iftype(value + 2, u64) {
    selected_expression
} else {
    fallback_expression
} @end
```

The checker infers the actual type of the arbitrary subject expression, compares it with the target type without numeric coercion, checks only the selected expression, and records that selection before LLVM lowering. This is generic semantic reflection for expressions such as `value + 2`; it is not yet deferred macro-arm selection or token generation. The form composes with ordinary `@using` expansions, so a generic macro can select a normal source conversion such as `std::string::String::from_u64($value)` without compiler knowledge of that macro or formatter. The unselected branch is never lowered.

The token-construction form is deliberately confined to a comptime boundary:

```shaft
@comptime emit {
    @using answer!() -> 42;
}
@end
```

It replaces the complete `@comptime emit { ... } @end` form with the balanced body tokens before ordinary hygienic `@using` discovery. The same boundary can select a token quotation with a literal condition:

```shaft
@comptime if (2 < 3) {
    @using answer!() -> 42;
} else {
    @using answer!() -> 0;
} @end
```

Literal `bool` conditions and one integer literal comparison (`==`, `!=`, `<`, `<=`, `>`, or `>=`) are evaluated before parsing; only the selected balanced body is emitted. A bounded literal token loop is also available:

```shaft
mut i32 answer = @comptime for (3) { 14 + } @end 0;
```

The loop repeats its balanced body exactly the supplied non-negative integer number of times, and can appear inside an ordinary expression as shown. To consume arbitrary forwarded macro arguments, a token-tree loop binds one balanced comma-separated argument at a time:

```shaft
@using sum!($tt:values...) ->
    @comptime for (tt value in $values) { $value + } @end 0;
```

The `tt` loop preserves each captured token tree exactly, including nested delimiters; `$value` splices the current tree into its token quotation. This is general argument iteration and carries no formatting or type-conversion policy. Dynamic identifier construction is also confined to an explicit comptime form:

```shaft
@using named!($ident:prefix) ->
    @comptime ident($prefix, _value) @end;
```

`@comptime ident(part, ...) @end` accepts one or more identifier fragments, joins their spellings into one identifier token, and rejects non-identifiers, missing commas, malformed terminators, and results longer than 4096 bytes. The constructed token retains the originating macro backtrace and is then processed through ordinary scope and macro resolution. For decoded source-literal inspection, the byte-loop form binds each UTF-8 byte and interpolates it into the quotation:

```shaft
@using byte_sum!($str:text) ->
    @comptime for (byte value in $text) { $value + } @end 0;
```

The literal-aware form accepts only a source string literal after ordinary macro substitution; `$value` is replaced with an integer literal from `0` through `255` for every decoded byte. The indexed form also binds a zero-based byte position:

```shaft
@using weighted_bytes!($str:text) ->
    @comptime for (byte value at position in $text) {
        $value * ($position + 1) +
    } @end 0;
```

`$position` is a deterministic per-iteration counter, allowing generated source to retain byte locations while it constructs tokens. The byte forms compose with nested literal token conditions, so a source macro can classify template punctuation without compiler naming special cases:

```shaft
@using template_score!($str:text) ->
    @comptime for (byte value in $text) {
        @comptime if ($value == 123) { 20 + } else { 22 + } @end
    } @end 0;
```

This deliberately exposes bytes rather than pretending UTF-8 bytes are Unicode scalar characters. Both loop forms and resulting token emission are capped by the normal 1,000,000-token safety ceiling; neither exposes host state. All forms can generate declarations, statements, or expression tokens; emitted tokens are still subject to normal parser, checker, macro-origin, and expansion-limit rules. `emit`, token `if`, and token `for` do not expose general host state, arbitrary string values, semantic type metadata, or shell access. Those require the general deterministic compile-time execution/capability model rather than host-process access.

### Standard `String` conversions

The standard prelude provides owned-string constructors for every primitive and for borrowed `std::string::view::View`: `std::string::String::from_i8`, `from_u8`, `from_i16`, `from_u16`, `from_i32`, `from_u32`, `from_i64`, `from_u64`, `from_usize`, `from_f32`, `from_f64`, `from_bool`, `from_char`, and `from_str`. Numeric and boolean conversion uses decimal / `true` / `false` text; `from_str` allocates and copies its input. The established `String::from_int(i64)` and `String::from_float(f64)` remain available.

`std::string::String::parse_i64`, `parse_u64`, and `parse_f64` convert strict decimal text back to a number. Each supplies a required `bool parsed` tunnel output and an optional numeric `value` output, so malformed text and integer overflow are never silently converted. Signed parsers accept one leading `-`; floating-point parsers accept an optional leading `-` and one decimal point (scientific notation is not accepted).

```shaft
reserve String count = String::from_u64(total);
reserve String nameCopy = String::from_str(name);

reserve bool parsed;
reserve ?i64 number;
text.parse_i64();
valid number { /* `number` is the i64 payload here */ }
```

All aliases search innermost scope first and must be unique within their declaring scope. Object-like recursive aliases and direct function-like `name!(...)` recursion are rejected. Replacement tokens may not contain `;`, `{`, or `}`, and are limited to 128 tokens; total generated expansion is limited to 1,000,000 tokens. Expansion is declaration-order-sensitive: a macro cannot refer forward to a later declaration.

## Memory and ownership vocabulary

```shaft
T destination = move source;
&T reader = ref source;
&mut T writer = ref source;
reserve T local;
reserve T local = expression;
reserve T local <- slot;
```

`move` transfers the ownership state of an addressable source into the destination. `ref` initializes a reference binding: `&T` creates a shared reader and `&mut T` creates the exclusive writer. Both accept an identifier or aggregate path such as `row.cells[0].value`. At a function call, an addressable argument is implicitly borrowed when its matching parameter is `&T` or `&mut T`, so `increment(value)` is equivalent to `increment(ref value)` for `def increment(&mut i32 value)`. Conversely, a reference argument is implicitly dereferenced for a non-reference parameter, so `verify(value)` supplies the `i32` behind `&i32 value` to `def verify(i32 value)`. An implicit `&mut` borrow still requires mutable caller storage. For fixed-size aggregate layout, the checker and backend track each field and fixed-array element independently: moving `row.cells[0]` leaves sibling fields/elements owned and eligible for compiler-inserted cleanup.

The checker is lexical and scope-releasing. It rejects use after move; direct root, field, or index writes while that root has any live reader or writer; moving an unknown/already-moved/live-borrowed owner; a writer while any reader or writer is live; and a reader while a writer is live. Multiple readers are permitted. An `&mut` borrow also requires a mutable source binding. A `ref` binding records the owning root of its path. When the binding's block ends, the checker releases that borrow record; a later move or write of the root is then valid. This applies recursively to structs, arrays, arrays of structs, and structs containing arrays. A nested reference conservatively keeps its root aggregate borrowed for the binding's full lexical lifetime, preventing an aliasing move or write through any enclosing aggregate path.

### Automatic scope cleanup

The language has no user-defined destructor or lifecycle hook. In particular, a method named `drop` inside an impl is rejected. Destruction is a compiler operation derived from storage layout, not a call back into source code.

On normal block exit and before `return`, `break`, and `continue`, the backend emits cleanup in reverse declaration order. It walks struct fields and fixed-size array elements in reverse layout order. Named runtime arrays `T[count]` snapshot their declaration-time count and maintain one compiler-side active ownership token per cleanup leaf of each element; scope exit traverses active leaves in reverse element/layout order without freeing their stack backing storage. A raw-pointer storage location is released through the compiler-private `__shaft_free` entry point only when it received a fresh `__shaft_alloc`/`__shaft_alloc_or_exit` result or an explicit `move`; ordinary pointer copies are borrowed aliases and have no cleanup right. This lets the same structural cleanup handle nested AoS layouts such as `Packet[4]`, structs containing fixed arrays, fixed arrays of structs, and cleanup-bearing named runtime-array elements without handwritten cleanup code. Bare runtime-sized `T[]` is a countless borrowed pointer form and receives no element destruction.

`move` transfers cleanup state from an addressable source into its destination, so compiler-inserted cleanup skips moved-from storage while still releasing retained siblings. A plain assignment to cleanup-owning storage conditionally releases its active previous value after evaluating the right-hand side and before storing the replacement, then rearms that target. Borrowed references (`&T`, `&mut T`) never release allocation metadata. Raw-pointer copies are borrowed aliases rather than independent owners.

`reserve` allocates a local without requiring a normal initializer. A Shaft `def` declares output slots as `-> T name`; prefix the arrow with `?` (`? -> T name`) when the slot may remain unfilled on a successful return. The backend supports plain reserve, expression initialization, and `<-` tunnel binding inside a Shaft `def`: `reserve mut T local <- slot;` aliases the declared output slot as local storage, so assigning `local` populates that slot. The bound local must be mutable when it is written, and its type must exactly match the slot type.

Raw pointers (`*T`) are explicit and unsafe at the language-design level; pointer validity remains the programmer's responsibility.

## Diagnostics and compilation behavior

The compiler reports source positions as one-based `line:column` values. Current primary formats are:

```text
Unknown token '@' at line L:C
Error at line L:C - parser message
Error [L:C]: checker message
Warning [L:C]: checker warning
```

`shaftc --check-only` stops after lexing, parsing, and checking. It is appropriate for CI and explicit editor-side validation. When its launcher provides an explicit compiler path and writable live-snapshot path, native `shaftls` invokes it for live diagnostics after structural checks pass; without those options, ShaftLS publishes structural brace diagnostics only.

Without `--check-only`, the compiler creates an LLVM module and emits one requested artifact. `--emit` accepts `llvm`, `object`, `asm`, `staticlib`, `dynamiclib`, and `binary`. `--target <LLVM-target-triple>` validates and selects an LLVM target for IR/native emission. Cross-target binary/static/dynamic linking still depends on a compatible external linker and runtime; target selection alone does not provide a cross-runtime.

## Editor protocol and integrations

`shaftls` is the native, dependency-free LSP server. It speaks JSON-RPC 2.0 over standard input/output using `Content-Length` framing; stdout is reserved for protocol frames. The server is tooling rather than a source-language semantic authority: it implements lifecycle, document open/change/close synchronization, semantic tokens, structural brace diagnostics, completion, and full-document formatting.

Install the server with `shaftc` and optionally install the shipped editor integrations:

```sh
python3 install.py --vscode --vim --neovim
```

The VS Code client starts `shaftls` from `PATH` unless `shaft.languageServer.serverPath` supplies an explicit executable. Vim starts `g:shaftls_cmd` when set, otherwise `shaftls`; Neovim starts `opts.cmd` when provided to `require('shaft').setup(opts)`, otherwise `shaftls`. The installed Neovim plugin calls that setup automatically. See the repository `README.md` for target paths and the VS Code prerequisites (`code`, `node`, and `zip`).

## Async states and named tasks

```shaft
def worker(*i32 output) {
    *output = 41;
}

// `State` is cooperative and executes on the caller.
State worker(&result) state;
start state;
await state;

// `Thread` starts `worker` concurrently; await joins it.
Thread worker(&result) workerThread;
start workerThread;
await workerThread;
```

`State call(args) name;` stores a deferred call. `start name;` runs that call once on the caller, and `await name` starts a not-yet-started state before observing its completion.

`Thread call(args) name;` captures a call at `start name;`, creates a native thread using the selected Linux, Darwin/macOS, or Windows runtime, and runs the target concurrently. Linux runtimes use `clone` and futex joining, Darwin/macOS uses `pthread_create`/`pthread_join` from `libSystem`, and Windows uses `CreateThread`/`WaitForSingleObject` from `kernel32`. `await name` joins that execution context; it also starts an unstarted thread before joining it. Scope cleanup also joins a started Thread before releasing its locals. Thread entry points must be non-generic free Shaft `def` functions with no tunnel outputs. The argument values are captured at `start`; pointer arguments still point to their original storage, which must remain valid until `await` or scope exit. Each handle starts once. A lexical Thread binding has one capture slot, so re-entering that binding while its previous invocation is still active terminates with status `72` rather than corrupting its captured arguments.

A named task block uses `identifier { ... }`. It remains an executable cooperative task unit: the block runs in deterministic program order, can use enclosing addressable values, and composes with `State`, `start`, and `await`.

## 12. Networking

`Network` provides a small synchronous IPv4 socket surface backed by every bundled runtime. `Network::ipv4(first, second, third, fourth)` builds a host-order IPv4 value from four octets; use it with `Network::tcp_connect_ipv4` rather than constructing platform `sockaddr` data in Shaft source.

```shaft
reserve Network::Socket client = Network::tcp_connect_ipv4(
    Network::ipv4(127, 0, 0, 1),
    8080
);
if (client.descriptor < 0) {
    exit(1);
}
reserve i64 sent = Network::send(&client, "ping");
*u8 response = __shaft_alloc(1024);
reserve i64 received = Network::receive(&client, response, 1024);
Network::close(&client);
```

`Network::send` loops until it has sent the entire `str` slice or encounters a native error, returning the total byte count. `Network::receive` makes one bounded native receive: a positive result is the received byte count, zero is peer EOF, and a negative result is a native socket error. `Network::close` is idempotent and invalidates `Socket.descriptor` after attempting the native close. `Network::tcp` and `Network::udp` create unconnected sockets; raw `Network::__sys_*` declarations remain available for explicitly unsafe protocol work and use byte pointers so platform-specific `sockaddr` layouts do not leak into the portable source ABI.

### Servers and nonblocking sessions

`Network::tcp_listen_ipv4(address, port, backlog)` creates a synchronous TCP listener and `Network::accept(&listener)` waits for one client. This is the simplest shape for a one-request-at-a-time HTTP host.

`Network::set_nonblocking(&socket)` switches a listener or connected socket to nonblocking mode. Then `Network::try_accept`, `Network::try_send`, and `Network::try_receive` return `-2` when their operation would block, allowing an application to poll multiple sessions without one idle peer stopping the loop. `try_send` may write only a prefix; retain the unsent suffix and retry it later. `State` is cooperative, while a supported native `Thread` can run an eligible void `def` concurrently; use nonblocking APIs when multiplexing is sufficient and Thread only when shared-state synchronization and native concurrency are deliberately designed.

## Tested Implementation

Currently implemented:

- C-ABI functions and forward ordinary function calls;
- integer and floating-point arithmetic/comparisons;
- assignments including tested compound assignments;
- `if`, `while`, `break`, and ordinary `return` from `cdef`;
- standard-prelude `String`, `readline`, `File::read`, `File::write`, `File::append`, nested generic syntax, and `Vector` use;
- private module macros plus standard-prelude `using global` output macros and explicit `using global` visibility across compiler source modules;
- immutable-by-default bindings and lexical shared/exclusive `&`/`&mut` borrowing;
- optional scalar values, optional copying, and `valid` presence branches;
- enum backing types plus explicit/computed member constants;
- `align N struct` local-storage alignment requests;
- concrete generic aggregate layouts and explicit generic `def` specialization;
- namespaced/qualified custom type identity;
- recursive struct/array/member lvalue resolution and two-level field mutability;
- lexical `ref` release for direct and nested aggregate paths;
- compiler-derived recursive destruction of raw-allocation storage in nested structs/fixed arrays across `move`, overwrite, `return`, `break`, and `continue` (manual `drop` methods are rejected);
- executable `async def`, `State`/`start`/`await`, and named task blocks;
- struct `@index`/`@init` declaration validation and indexing layout;
- Shaft-source `Collections::HashMap` and `Collections::HashSet` behavior;
- LLVM IR, object, assembly, static library, dynamic library, and native binary artifact production.

The portable suite exercises the documented native surface through executable and early-rejection regressions; parser recognition alone is never treated as a runtime feature.

## 14. Benchmark protocol

`benchmarks/run.py` is a reproducible three-way implementation of the Computer Language Benchmarks Game `mandelbrot` escape-count kernel for Shaft, Clang, and Rust. It records source hashes, tool versions, compiler commands, per-sample timings, median/mean/minimum/maximum, output binary size, and a checked escape-count checksum. By default it pins itself and benchmark children to one eligible CPU, warms each executable once, and interleaves runtime samples in rotating tool order. Use `--no-pin` only when affinity control is unsuitable for the host.

The harness reports LLVM-IR generation, native-build latency, and executable runtime as separate measurements. Every runtime sample must return the generated workload's expected checksum exit code before its timing is accepted. Keep generated JSON results outside the repository unless a machine-specific baseline is explicitly requested; equivalent workload parameters, native optimization policy, and recorded source hashes are required when comparing a later result.
