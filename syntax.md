# Shaft language specification

## Lexical structure

### Whitespace, comments, and identifiers

Whitespace separates tokens. Line comments begin with `//`; block comments use `/* ... */`; documentation comments written as `///` are comments to the compiler and are consumed by the VS Code tooling for hover documentation.

Identifiers are distinct from keywords. Qualified identifiers use `::`:

```shaft
Collections::HashMap
Core::answer
```

The lexer recognizes the keywords listed in the compiler header, including `def`, `dec`, `cdef`, `cdec`, `struct`, `class`, `enum`, `namespace`, `using`, `global`, `import`, `export`, `if`, `while`, `for`, `foreach`, `return`, `tunnel`, `reserve`, `move`, `ref`, and the primitive type names.

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

`usize` is lowered to an unsigned integer matching the selected target's native pointer width. `State` and `Thread` both use the executable cooperative deferred-call semantics described in §11. A `Thread` binding is a deterministic task handle, not a host OS thread.

### Compound and named types

The grammar accepted for types is:

```text
type           ::= type-prefix* type-base array-suffix*
type-prefix    ::= "mut" | "*" ["mut"] | "&" ["mut"] | "?"
type-base      ::= primitive-type | qualified-name ["<" type ("," type)* ">"]
array-suffix   ::= "[" [integer-literal | identifier] "]"
qualified-name ::= identifier ("::" identifier)*
```

Examples:

```shaft
* i8 rawBytes;
&mut u64 writer;
?String maybeName;
u8[16] fixed;
String[] argv;
Collections::HashMap<String, u64> counts;
```

A fixed-size array `T[N]` lowers to an LLVM array. A named runtime array `T[name]` requires a previously declared `u64` or `usize` local length binding; the allocation snapshots that count, and named runtime arrays cannot be struct or class fields. `T[]` is the borrowed pointer-style form used where a separate count is supplied by the surrounding ABI. Pointer and reference types both lower as opaque LLVM pointers. References carry checker-enforced lexical aliasing: one `&mut T` writer is exclusive, while any number of `&T` readers may coexist; readers and a writer may not coexist.

Qualified custom type identity is canonical: `Left::Token` and `Right::Token` are distinct types.

Optional values lower to a tagged LLVM aggregate containing a presence flag and a payload. `?T value = expression;` creates a present value, while an uninitialized `?T value;` is absent. `valid value { ... } else { ... }` branches on that flag; value reads in the valid branch use the payload. Optional copy preserves both flag and payload.

### Generic syntax

Structs, classes, and Shaft `def`/`dec` declarations accept simple name-only generic parameter lists. Named types and explicit generic calls accept type arguments. Nested closing `>>` is accepted as two generic closers.

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
               ["<" generic-parameters ">"] tunnel-slots ";"
tunnel-slots ::= [ ["?"] "->" type identifier ]
```

```shaft
def add(i32 lhs, i32 rhs) -> i32 result {
    tunnel lhs + rhs -> i32 result;
}
```

A non-optional tunnel slot must be populated on every checked path. The checker rejects missing required slots, unknown slot names, type mismatches, and a slot that may be written more than once. `tunnel value -> T slot;` must restate the slot type syntactically; the checker compares it with the declared slot type. `async def` and `async dec` are cooperative callable declarations: they use the same native tunnel ABI as `def`/`dec` and can be scheduled through `State` or `Thread` with `start` and `await`.

`def` lowers as an LLVM `void` function with tunnel results passed as output pointers. Optional results additionally receive native presence-flag output pointers; an unfilled result produces an absent optional, while a tunneled value produces a present optional with its payload. A single tunnel may be used as a call expression. Bind multiple required outputs exactly once with `reserve T first, U second = call();`; each binding receives its corresponding slot in declaration order. A direct `__shaft_alloc` or `__shaft_alloc_or_exit` result, or an aggregate containing compiler-owned allocation storage, cannot be tunneled because tunnel payloads do not yet transport ownership metadata.

`return` is invalid in `def` and `dec`; use `tunnel`.

### C-ABI functions: `cdef` and `cdec`

```text
c-definition ::= "cdef" identifier "(" parameters? ")" ["->" type] block
c-declaration ::= "cdec" identifier "(" parameters? ")" ["->" type] ";"
parameters    ::= parameter ("," parameter)*
parameter     ::= type identifier
```

```shaft
cdef add(i32 lhs, i32 rhs) -> i32 {
    return lhs + rhs;
}

cdec memcpy(*i8 destination, *i8 source, u64 bytes) -> *i8;
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
cdec InitWindow(i32 width, i32 height, *i8 title);
cdec WaitTime(f64 seconds);
cdec CloseWindow();

cdef main() -> i32 {
    InitWindow(640, 360, "Shaft + raylib");
    WaitTime(1.0);
    CloseWindow();
    return 0;
}
```

Build it against a host-installed raylib:

```text
shaftc --no-std --hosted -lraylib raylib-window.shaft
```

Use `--link-dir /path/to/lib` when raylib is outside the system linker paths. C declarations must match the library's C ABI exactly; model C `bool` as `i32` unless the target API documents another integer width.

`def main` is emitted as `__main`. The bundled freestanding entry bridge converts process arguments to `String[]`, calls `__main`, exits with `0` on normal completion, and honors `exit(N)` for a nonzero process status. This bridge requires the standard library/runtime resources; it is not enabled by `--no-std` alone.

### Methods and `self`

Struct/class bodies may contain function declarations and definitions. A first parameter may be:

```shaft
self
&self
&mut self
```

The parser resolves `Self` relative to the containing structure. The backend lowers method names using the containing class/namespace name and supports direct receiver method calls for addressable receivers. A class may derive from one earlier-declared class with `class Derived : Base { ... }`; the derived native layout begins with its inherited fields, so inherited members participate in ordinary field access and mutation. Class construction uses the existing brace-initialization surface.

## Control flow

### Executable control flow

```shaft
if (condition) { ... } else { ... }
while (condition) { ... }
break;
continue;
```

`else if` is parsed as an `else` containing another `if`. `break` and `continue` are checker errors outside a loop. `if` and `while`, including `break` and `continue`, are lowered by the backend and covered by the compiler smoke tests.

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

### Structs and classes

```text
struct-declaration ::= "struct" ["<" generic-parameters ">"] identifier struct-body
class-declaration  ::= "class"  ["<" generic-parameters ">"] identifier class-body
aligned-struct     ::= "align" integer-literal "struct" ["<" generic-parameters ">"] identifier struct-body
```

```shaft
struct Point {
    i32 x;
    i32 y;
}

class Buffer {
    *u8 data;
    u64 length;
    index data;
    init data;
}
```

Structs and classes share the bootstrap layout representation. Fields and methods are collected from the body. A field assignment is mutable only when both its root struct/class binding and every traversed field are declared `mut`:

```shaft
struct Cell { mut i32 value; }
struct Grid { mut Cell[2] cells; }

mut Grid grid;
grid.cells[0].value = 42;
```

The checker resolves this recursively through members and arrays. Removing `mut` from `grid`, `cells`, or `value` makes that assignment a checker error. `align N struct` requires a nonzero power-of-two `N`; the backend preserves it and applies it to local LLVM allocations of that struct type.

Class `index field;` declares the direct backing field used by `receiver[index]`. It is valid only once and only when `field` is a pointer or runtime-sized array. `init field;` is also unique and must name the same field as `index`. The backend lowers both index access and brace construction for an `init`-configured class by allocating its backing storage through `__shaft_alloc`.

```shaft
mut Vector<i64> values = { 40, 0 };
```

Plain positional struct literals use the same surface syntax and initialize declared fields in source order. The initializer must supply exactly one value for every field, with each value convertible to its corresponding field type. Classes that declare an `init` backing field retain their allocation-backed brace-construction behavior.

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
namespace Collections {
    class HashMap<K, V> { ... }
}
```

Namespaces create checker/code-generation qualification contexts. Nested names use `::` in source type identity. The current backend uses dot-separated LLVM symbol components for namespace/class function names. This is an implementation ABI detail, not source syntax.

### Export and import

```shaft
export cdef api(i32 value) -> i32 { return value; }
import "module.shaft";
```

`export` is an executable transparent declaration wrapper: the backend lowers its enclosed declaration normally. `import "module.shaft";` loads a Shaft source module relative to the importing file. Imports are compile-time dependency directives: each canonical file is loaded once, cycles are rejected, and dependencies compile before their importers. Imported declarations share the project-wide checker/code-generation namespace.

### `using` macros

```shaft
using alias -> replacement;
using "name!" -> replacement;
using call!(left, right) -> expression;
using global public_call!(value) -> expression;
```

A `using` macro is a scoped lexer expansion. Object-like macros use an identifier or string alias, `->`, then a non-empty token replacement ending in `;`. Function-like macros use an identifier followed by `!`, a comma-separated identifier parameter list, and an expression-token replacement. They are invoked as `call!(arguments...)`; argument token sequences are substituted for matching replacement parameters, with nested parenthesized calls preserved. Invocation arity must exactly match its declaration.

`using` is private to its source module and lexical scope. `using global` exports an alias to later compiler source modules. The standard prelude uses `using global` for its public output convenience macros: `print!` and `println!`; they are available from application source. Its other internal macros remain private. Local declarations shadow global ones. An invocation of an unavailable `name!(...)` is a lexer error, preventing accidental fallback to an ordinary expression.

### Compiler-provided `printf`

With the standard prelude enabled (the default), `printf` is a compiler-provided formatting intrinsic, not a declaration from `std`:

```shaft
printf("Hello {str}!\\n", name);
printf("count={u64}, value={f64}\\n", count, value);
```

Its first argument must be a string literal. Each `{type}` placeholder consumes one following argument and must exactly name its type: `i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `i64`, `u64`, `usize`, `f32`, `f64`, `bool`, `char`, `str`, or `String`. It writes literal segments and formatted arguments directly to standard output and returns the number of bytes written. It is intentionally unavailable under `--no-std`; malformed placeholders, mismatched types, and argument-count mismatches are compilation errors.

### Standard `String` conversions

The standard prelude provides owned-string constructors for every primitive and for borrowed `str`: `String::from_i8`, `from_u8`, `from_i16`, `from_u16`, `from_i32`, `from_u32`, `from_i64`, `from_u64`, `from_usize`, `from_f32`, `from_f64`, `from_bool`, `from_char`, and `from_str`. Numeric and boolean conversion uses decimal / `true` / `false` text; `from_str` allocates and copies its input. The established `String::from_int(i64)` and `String::from_float(f64)` remain available.

`String::parse_i64`, `parse_u64`, and `parse_f64` convert strict decimal text back to a number. Each supplies a required `bool parsed` tunnel output and an optional numeric `value` output, so malformed text and integer overflow are never silently converted. Signed parsers accept one leading `-`; floating-point parsers accept an optional leading `-` and one decimal point (scientific notation is not accepted).

```shaft
reserve String count = String::from_u64(total);
reserve String nameCopy = String::from_str(name);

reserve bool parsed;
reserve ?i64 number;
text.parse_i64();
valid number { /* `number` is the i64 payload here */ }
```

All aliases search innermost scope first and must be unique within their declaring scope. Object-like recursive aliases and direct function-like `name!(...)` recursion are rejected. Replacement tokens may not contain `;`, `{`, or `}`, and are limited to 128 tokens; total generated expansion is limited to 1,000,000 tokens. Expansion is declaration-order-sensitive: a macro cannot refer forward to a later declaration. These constraints are covered by compiler tests.

## Memory and ownership vocabulary

```shaft
T destination = move source;
&T reader = ref source;
&mut T writer = ref source;
reserve T local;
reserve T local = expression;
reserve T local <- slot;
```

`move` transfers the ownership state of an addressable source into the destination. `ref` initializes a reference binding: `&T` creates a shared reader and `&mut T` creates the exclusive writer. Both accept an identifier or aggregate path such as `row.cells[0].value`. For fixed-size aggregate layout, the checker and backend track each field and fixed-array element independently: moving `row.cells[0]` leaves sibling fields/elements owned and eligible for compiler-inserted cleanup.

The checker is lexical and scope-releasing. It rejects use after move; direct root, field, or index writes while that root has any live reader or writer; moving an unknown/already-moved/live-borrowed owner; a writer while any reader or writer is live; and a reader while a writer is live. Multiple readers are permitted. An `&mut` borrow also requires a mutable source binding. A `ref` binding records the owning root of its path. When the binding's block ends, the checker releases that borrow record; a later move or write of the root is then valid. This applies recursively to structs, arrays, arrays of structs, and structs containing arrays. A nested reference conservatively keeps its root aggregate borrowed for the binding's full lexical lifetime, preventing an aliasing move or write through any enclosing aggregate path.

### Automatic scope cleanup

The language has no user-defined destructor or lifecycle hook. In particular, a method named `drop` inside a struct or class is rejected. Destruction is a compiler operation derived from storage layout, not a call back into source code.

On normal block exit and before `return`, `break`, and `continue`, the backend emits cleanup in reverse declaration order. It walks struct/class fields and fixed-size array elements in reverse layout order. Named runtime arrays `T[count]` snapshot their declaration-time count and maintain one compiler-side active ownership token per cleanup leaf of each element; scope exit traverses active leaves in reverse element/layout order without freeing their stack backing storage. A raw-pointer storage location is released through the compiler-private `__shaft_free` entry point only when it received a fresh `__shaft_alloc`/`__shaft_alloc_or_exit` result or an explicit `move`; ordinary pointer copies are borrowed aliases and have no cleanup right. This lets the same structural cleanup handle nested AoS layouts such as `Packet[4]`, structs containing fixed arrays, fixed arrays of structs, and cleanup-bearing named runtime-array elements without handwritten cleanup code. Bare runtime-sized `T[]` is a countless borrowed pointer form and receives no element destruction.

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

`shaftc --check-only` stops after lexing, parsing, and checking. It is appropriate for editor diagnostics; use a normal compile to validate target-specific linking and artifact emission.

Without `--check-only`, the compiler creates an LLVM module and emits one requested artifact. `--emit` accepts `llvm`, `object`, `asm`, `staticlib`, `dynamiclib`, and `binary`. `--target <LLVM-target-triple>` validates and selects an LLVM target for IR/native emission. Cross-target binary/static/dynamic linking still depends on a compatible external linker and runtime; target selection alone does not provide a cross-runtime.

## Async states and named tasks

```shaft
def worker(*i32 output) {
    *output = 41;
}

State worker(&result) state;
start state;
await state;

// `Thread` uses the same deterministic cooperative lifecycle.
Thread worker(&result) workerThread;
start workerThread;
await workerThread;

workerTask {
    result = result + 1;
}
```

`State call(args) name;` and `Thread call(args) name;` store a deferred call. `start name;` runs that call once. `await name` observes completion and starts a not-yet-started task before observing it, so both forms have a single deterministic execution point. They are checker-visible lexical bindings and are released when their enclosing scope ends. `Thread` deliberately has no host-thread, scheduler, or libc dependency: it is the named cooperative-handle spelling.

A named task block uses `identifier { ... }`. It is an executable cooperative task unit: the block runs in deterministic program order, can use enclosing addressable values, and composes with `State`, `start`, and `await` without a libc or host-thread dependency. The native regression executes a `def` through a `State`, awaits it, executes a named task block, and exits with `42`.

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

`Network::set_nonblocking(&socket)` switches a listener or connected socket to nonblocking mode. Then `Network::try_accept`, `Network::try_send`, and `Network::try_receive` return `-2` when their operation would block, allowing an application to poll multiple sessions without one idle peer stopping the loop. `try_send` may write only a prefix; retain the unsent suffix and retry it later. These calls are async-ready, not hidden parallelism: Shaft's `State`/`Thread` execution is deterministic and cooperative, so an `async def` cannot itself make a blocking host syscall concurrent or faster. Use the `try_*` API for responsiveness/multiplexing; add an OS event loop or host-thread runtime only when true parallel I/O is deliberately designed and implemented.

## 13. Test-backed portable subset

The current smoke/integration suite compiles and executes programs covering:

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
- class `index`/`init` declaration validation and indexing layout;
- Shaft-source `Collections::HashMap` and `Collections::HashSet` behavior;
- LLVM IR, object, assembly, static library, dynamic library, and native binary artifact production.

The portable suite exercises the documented native surface through executable and early-rejection regressions; parser recognition alone is never treated as a runtime feature.

## 14. Benchmark protocol

`benchmarks/run.py` is the reproducible three-way performance harness for Shaft, Clang, and Rust. It records source hashes, tool versions, compiler commands, per-sample timings, median/mean/minimum/maximum, and output binary size. By default it pins itself and benchmark children to one eligible CPU, warms each executable once, and interleaves runtime samples in rotating tool order. Use `--no-pin` only when affinity control is unsuitable for the host.

The harness reports LLVM-IR generation, native-build latency, and executable runtime as separate measurements. Results in `benchmarks/results/bench1.md` are a machine-specific baseline, not a universal performance claim. Equivalent workloads, native optimization policy, and the recorded source hashes are required when comparing a later result.
