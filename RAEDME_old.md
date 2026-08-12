# ThaiThon

ThaiThon compiles a small imperative language to LLVM IR (`.ll`), which is
then run through `llc` + `clang` to produce assembly or a native
executable. This revision adds a real language surface — variables,
control flow, functions, classes, a small standard library, and JSON/file
I/O — on top of the project's existing C/C++ FFI mechanism.

```
Source (.tt) → Lexer (Mono_c11.hpp) → Parser (Parser.hpp) → LLVM IR (.ll)
                                                                  │
                                                    llc            │  clang
                                                     │              │
                                                     ▼              ▼
                                                  assembly      object file(s)
                                                   (.s)           │
                                                                   ▼ clang (link)
                                                              executable
```

## Files in this project

| File | What it does |
|---|---|
| `main.cpp` | CLI + toolchain driver: lex → parse → emit `.ll` → (optionally) `opt`/`llc`/`clang` |
| `Parser.hpp` | The compiler front-end: hand-written recursive-descent parser + code generator |
| `ModuleWriter.hpp` | LLVM-IR text assembly (declares, globals, structs, multiple `define`d functions, temp/label allocation) |
| `Runtime.cpp` | C++ runtime, `extern "C"`, linked into every executable — backs `print`/`input`/`split`/`readfile`/`json*`/etc. |
| `Mono_c11.hpp` | Lexer + token definitions (shared with the `mono` grammar tool; only the `Lexer`/`Token` parts are used here) |
| `ThaiThonRule.json` | Lexer configuration (operators/symbols) consumed by `Mono_c11.hpp`'s `load_rule()` |
| `lib_header.json` | Registry of importable libraries for the C/C++ FFI (`import "name";`) |
| `mymath.c` | Example C source linked in via `lib_header.json`'s `mymath` entry |
| `examples/*.tt` | Sample programs — see below |

> **Not build-tested here.** This repository only shared `Parser.hpp`,
> `main.cpp`, and `mono.hpp`'s public shape with me — `Executor.hpp` and the
> full `Mono_c11.hpp` weren't included, so I couldn't actually compile this
> in the sandbox. I've kept everything as close as possible to the style
> and conventions of the code you showed me, but build it in your own tree
> and expect to fix a small integration issue or two.

## Building

```sh
g++ -std=c++17 -O2 main.cpp -o thaithon
```

You'll also need LLVM's `llc`, `opt` (optional), and `clang` on your
`PATH` (or pass `-llc=`, `-clang=`, `-opt-bin=` to point at them) for
anything beyond `-target=ir`.

## Compiling a program

```sh
./thaithon -m=c -i=examples/basics.tt -target=exe
./program            # or ./basics on some setups — see -o below
```

- `-target=ir` (default) — emit `<input>.ll` only
- `-target=asm` — emit `<input>.s` via `llc`
- `-target=exe` — emit a linked, executable binary via `llc` + `clang`
  (this is the only mode that automatically links `Runtime.cpp` and any
  FFI `source` files, so built-ins and imported C/C++ libraries only work
  end-to-end here — see the warnings the compiler prints for the other
  two modes)
- `-o=<path>` — override the output path
- `-opt=O1|O2|O3|Os|Oz` — run LLVM `opt` on the IR before `llc`
- `-keep-ir=true` — keep the intermediate `.ll` file around

## Language reference

### Variables — `let` / `const`

```
let int x = 5;          // explicit type
let y = 5;               // auto-inferred from the initializer
const string name = "ThaiThon";
```

Supported types: `int` (32-bit), `float` / `double` (both are the same
64-bit IEEE-754 type internally — see "Design notes" below), `string`,
`bool`, `char`, `json`, and any `class` you've declared. `auto`/omitting
the type both mean "infer from the right-hand side".

`const` is enforced at compile time — reassigning one is a parse error.
Reassigning a `let` is just `x = expr;`.

### Control flow

```
if (a < b) {
    ...
} else if (a == b) {
    ...
} else {
    ...
}

while (i < 10) {
    i = i + 1;
}
```

Conditions are a single comparison (`==`, `!=`, `<`, `>`, `<=`, `>=`) or a
bare `bool`/`int`/`char` expression (non-zero is truthy). There's no
`&&`/`||` yet — see "Not implemented" below.

### Functions

```
function add(int a, int b) int {
    return a + b;
}

function greet(string name) void {
    print("Hi, ");
    println(name);
}
```

The return type comes after the parameter list; omit it for a `void`
function. Functions must be declared **before** they're called (single
top-to-bottom pass, like most scripting languages — not two-pass like C).
No overloading, no default arguments, no varargs.

### Classes

```
class Point {
    int x;
    int y;
}

let p = new Point();
p.x = 1;
p.y = 2;
```

Classes are **fields only** — no methods, no inheritance, no
constructors (`new ClassName()` zero-argument-allocates and
zero-initializes nothing; set fields explicitly after). Instances are
heap-allocated (`malloc`) and never freed — fine for short scripts, not
for long-running processes. Pass an instance to a function to get
method-like behavior:

```
function move(Point p, int dx, int dy) void {
    p.x = p.x + dx;
    p.y = p.y + dy;
}
```

### Built-ins

All backed by `Runtime.cpp`, linked automatically for `-target=exe`:

| Built-in | Signature | Notes |
|---|---|---|
| `print(a, b, ...)` | polymorphic | prints each argument (space-separated), no trailing newline |
| `println(a, b, ...)` | polymorphic | same, plus a trailing newline |
| `input(prompt)` | `string(string)` | prompt may be `""`; returns `""` at EOF |
| `readfile(path)` | `string(string)` | returns `""` if the file can't be read |
| `writefile(path, content)` | `int(string, string)` | returns `1`/`0` |
| `split(str, delim)` | `array(string, string)` | see below |
| `arraylen(arr)` | `int(array)` | |
| `arrayget(arr, i)` | `string(array, int)` | out-of-range → `""` |
| `strconcat(a, b)` | `string(string, string)` | same as `a + b` on two strings |
| `streq(a, b)` | `bool(string, string)` | same as `a == b` on two strings |
| `strlen(s)` | `int(string)` | |
| `jsonparse(text)` | `json(string)` | |
| `jsonreadfile(path)` | `json(string)` | |
| `jsonwritefile(j, path)` | `int(json, string)` | pretty-printed |
| `jsonstringify(j)` | `string(json)` | compact |
| `jsonnew()` | `json()` | new empty JSON object |
| `jsonset{string,int,double,bool}(j, key, val)` | `void(json, string, T)` | |
| `jsonget{string,int,double,bool}(j, key)` | `T(json, string)` | missing/wrong-type key → zero value |
| `jsongetobject(j, key)` | `json(json, string)` | |
| `jsonhas(j, key)` | `bool(json, string)` | |

`split()`'s return value and `json` handles are opaque `ptr`s from
ThaiThon's point of view — you can store them in a `let`/pass them
around, but there's no `[]` indexing syntax; use `arrayget`/the `json*`
accessors.

### Connecting to C/C++

This was already supported before this revision and is unchanged:
`lib_header.json` maps a library name to LLVM `declare`s, an optional
list of `source` files to compile+link, and a `functions` table mapping
a ThaiThon-visible name to the real symbol, return type, and (optionally)
a typed `params` list:

```json
"mymath": {
  "source": ["mymath.c"],
  "declare": ["declare i32 @add_numbers(i32, i32)"],
  "functions": {
    "add": { "symbol": "add_numbers", "returns": "i32", "params": ["i32", "i32"] }
  }
}
```

```
import "mymath";
let int r = mymath.add(3, 4);
println(r);
```

- `import "name";` pulls in a library's `declare`s (and, on first import,
  registers its `source`/`libs` for the link step).
- `module.function(args)` can now be used **either as a statement or as
  an expression** (this revision added the expression form so you can do
  `let r = mymath.add(3, 4);`).
- Call arguments to FFI functions must still be literals (int/float/
  string) — passing a ThaiThon variable straight into an FFI call isn't
  wired up (same limitation as before this revision).
- `.cpp` sources need their linked functions wrapped in
  `extern "C" { ... }` so the symbol names aren't mangled — see
  `Runtime.cpp` for a real example of this pattern.
- A project can drop its own `lib.json` (same schema) next to its `.tt`
  file (or in the CWD) instead of editing the compiler's shared
  `lib_header.json` — see `mergeProjectLibFile()` in `Parser.hpp`.

`Runtime.cpp`'s built-ins are just this same mechanism used internally —
`Parser.hpp`'s `registerBuiltins()` is effectively a hardcoded
`lib_header.json` entry for a library that's always "imported".

## How the compiler works (for anyone extending this)

- **No SSA / mem2reg.** Every `let`/`const`/function parameter is an
  `alloca` stack slot; every read is a `load`, every write is a `store`.
  This is deliberately the simplest correct thing — valid LLVM IR that
  `llc` accepts as-is, just not optimal. Run `-opt=O1` (or higher) if you
  care about the generated code's quality; LLVM's `mem2reg` pass cleans
  this pattern up completely.
- **Everything not obviously scalar is `ptr`.** Strings, `json` handles,
  `split()`'s array handles, and class instances are all the LLVM opaque
  `ptr` type at the machine level. ThaiThon's static type (`string` vs
  `json` vs a class name) is tracked only inside `Parser.hpp`'s
  `ExprResult`/`VarInfo`, purely to type-check and to pick the right
  built-in overload — it doesn't exist at the LLVM IR level.
- **`bool`-returning built-ins declare `i8`, not `i1`, at the LLVM
  boundary.** Declaring an extern C function as returning LLVM `i1`
  when the real C++ symbol returns `int8_t` is an ABI mismatch waiting
  to happen; the built-in table in `registerBuiltins()` declares those
  as `i8` and `Parser.hpp`'s `ensureBool()` does the `icmp ne ..., 0`
  truncation to `i1` wherever a real boolean is needed (an `if`/`while`
  condition). ThaiThon's own `bool` type/literals are still native `i1`.
- **Classes are named LLVM struct types** (`%ClassName = type { ... }`,
  registered via `ModuleWriter::addStruct`), and `new ClassName()` uses
  the standard "GEP-on-null" `sizeof` idiom to compute the byte size to
  `malloc`, so field alignment/padding is whatever LLVM's own struct
  layout gives you — no manual offset math.
- **Functions are top-to-bottom declared-before-use, single pass.**
  `Parser.hpp` doesn't pre-scan the file for signatures; simpler, at the
  cost of forward references not being supported. If you want mutual
  recursion or forward references, add a first pass that walks the
  token stream collecting `function`/`class` signatures before the real
  codegen pass runs.
- **Fixed two bugs found while reading the original file:** a stray
  empty `class Parser { };` forward-stub before the real class
  definition (this is a hard compile error in C++ — two definitions of
  the same class in one translation unit) has been removed, and the
  broken `braceStatement()` (`Parser parser(this->tokens,);` — a
  trailing comma with no `MotherPath` argument) has been removed along
  with the unused `bracketStatement()`; their functionality is
  superseded by the new `block()`/`classDecl()`/`funcDecl()`.

## Not implemented (intentionally, to keep this a reviewable revision)

- `&&` / `||` / `!` — one comparison per `if`/`while` condition only
- `[]` array-literal syntax and indexing — use `split()` +
  `arraylen()`/`arrayget()`
- Class methods, inheritance, constructors with arguments
- Closures / nested function declarations
- Passing a ThaiThon variable (vs. a literal) as an argument to an FFI
  (`module.function(...)`) call
- Garbage collection / `free()` — `new` and the string-returning
  built-ins leak by design (see "Design notes"); fine for scripts, not
  for long-running services
- A real two-pass compiler (forward references to functions/classes
  declared later in the file)

## Examples

- `examples/basics.tt` — variables, `if`/`else if`/`else`, `while`,
  `print`/`println`
- `examples/functions_classes.tt` — `function`, `class`, field
  get/set, calling a function with an object argument
- `examples/split_input.tt` — `input`, `split`, `arraylen`/`arrayget`
- `examples/json_files.tt` — building/reading JSON, `readfile`/
  `writefile`

```sh
./thaithon -m=c -i=examples/functions_classes.tt -target=exe -o=examples/functions_classes
./examples/functions_classes
```
