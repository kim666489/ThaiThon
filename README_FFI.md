# Link C / C++ functions into ThaiThon

This adds the ability to write real C or C++ functions, compile them, and
call them from ThaiThon source — without touching the lexer/grammar at all.
It's built entirely on the `import "name";` / `name.func(args);` syntax
that already existed.

## Files changed
- `Parser.hpp` — `importStatement()` now also reads an optional `"source"`
  list from `lib_header.json` and remembers those C/C++ file paths.
  `callStatement()` now supports a `"params"` list per function so calls
  can pass typed int/float/string arguments, not just a single string.
- `main.cpp` — when `-target=exe`, every linked source file is compiled
  with `clang -c` and the resulting `.o` files are linked together with
  the ThaiThon program's own `.o` into one executable.
- `lib_header.json` — two new optional keys per library: `"source"` and
  `"params"` (see `mymath` entry in the example file).

## How to link your own C/C++ function

1. Write the function in C (or C++ wrapped in `extern "C" { ... }`):

   ```c
   // mymath.c
   int add_numbers(int a, int b) { return a + b; }
   ```

2. Add an entry to `lib_header.json`, next to your existing libraries:

   ```json
   "mymath": {
     "source": ["mymath.c"],
     "libs": ["m"],
     "declare": ["declare i32 @add_numbers(i32, i32)"],
     "functions": {
       "add": { "symbol": "add_numbers", "returns": "i32", "params": ["i32", "i32"] }
     }
   }
   ```

   - `source` — paths to your `.c`/`.cpp` files (relative to the folder
     that holds `lib_header.json`, or absolute).
   - `libs` — *optional*. System libraries the linker needs `-l<name>` for.
     Needed whenever your C/C++ code calls into a library that isn't part
     of plain libc, e.g. `"libs": ["m"]` for `<math.h>` functions like
     `sqrt`/`pow` (on Linux/WSL these live in libm, not libc), or
     `"libs": ["pthread"]` for POSIX threads.
   - `declare` — the LLVM `declare` line for the function, must match the
     C signature exactly.
   - `functions.<name>.symbol` — the C symbol name (must match the
     function name in the `.c`/`.cpp` file).
   - `functions.<name>.params` — LLVM types in order: `ptr` (string),
     `i32`/`i64`/`i8`/`i16`/`i1` (integers), `float`/`double`.

3. Call it from ThaiThon:

   ```
   import "mymath";
   mymath.add(3, 4);
   ```

4. Build with linking turned on:

   ```
   ThaiThon -m=c -i=example.tt -target=exe
   ```

   This compiles `example.tt` to IR, compiles `mymath.c` to an object file,
   and links both into a single executable. `-target=ir` / `-target=asm`
   still work but won't auto-link extra sources (a warning is printed).

## Reverse FFI: C/C++ calling ThaiThon functions

The compiler also supports the reverse direction: a ThaiThon function is
emitted as a normal LLVM function definition, so a C/C++ program can link
against it as long as the symbol is declared with the right ABI.

1. Write the ThaiThon function:

   ```thai
   function add(int a, int b) int {
       return a + b;
   }
   ```

2. Compile it to LLVM IR or object code:

   ```bash
   ./bin/thaithon.out -m=c -i=export.tt -target=ir
   llc -filetype=obj export.ll -o export.o
   ```

   The function is emitted as a regular LLVM symbol such as:

   ```llvm
   define i32 @add(i32 %a, i32 %b) {
   ...
   }
   ```

3. Declare it from C++ with `extern "C"` and call it:

   ```cpp
   extern "C" int add(int a, int b);

   int main() {
       int x = add(10, 32);
       return x == 42 ? 0 : 1;
   }
   ```

4. Link both objects together:

   ```bash
   g++ main.cpp export.o -o main
   ```

This works because ThaiThon function names are emitted in plain LLVM/C
symbol form, not C++-mangled names. If you use C++ code, you must wrap the
prototype in `extern "C"` to prevent name mangling.

## Current limitations (kept intentionally small in scope)
- Call arguments must be literals (`3`, `"hi"`, `2.5`) — passing a
  ThaiThon *variable* into a linked function isn't wired up yet; this
  hand-rolled `Parser` doesn't track variable → register mappings (that
  lives in the separate `mono.hpp` Normalizer pipeline).
- Supported param types: `ptr`, `i1/i8/i16/i32/i64`, `float`, `double`.
  Structs/pointers-to-structs aren't supported.
- If a library entry has no `"params"`, the old single-string-argument
  behavior is used unchanged — existing `lib_header.json` files (like a
  `stdio`-only one) keep working with zero edits.
- For reverse FFI, you need a real LLVM toolchain (`llc`, `clang`, or `opt`)
  available in the environment when building the final object/binary.

## Project-local `lib.json` — link libraries without touching the compiler folder

Editing `lib_header.json` inside the compiler's own install folder every
time you want to link something gets old fast. Instead, drop a `lib.json`
(same schema as `lib_header.json` — see `lib.json` / `greet.c` in this
bundle for a full example) either:

- in the current working directory you run the compiler from, or
- in the same folder as the `.tt` file you're compiling (`-i=...`)

Both locations are checked automatically, no flag needed. The compiler
merges `lib.json` on top of `lib_header.json` using [JSON Merge
Patch](https://datatracker.ietf.org/doc/html/rfc7386) semantics:

- A brand-new library key → just gets added.
- An existing library key → merged field-by-field. Fields you don't
  mention are left as-is; fields you do mention (e.g. adding one more
  entry to `"functions"`) win and get merged in, without disturbing
  anything else already declared for that library.
- `"source"` paths inside a project's `lib.json` resolve relative to
  **that file's own folder** (not the compiler's folder), so your `.c`/
  `.cpp` files can just live next to `lib.json` in the project.

Example project layout:

```
myproject/
  program.tt
  lib.json      <- your own libraries, e.g. "greet" -> greet.c
  greet.c
```

```json
// myproject/lib.json
{
  "greet": {
    "source": ["greet.c"],
    "declare": ["declare void @say_hi(ptr)"],
    "functions": {
      "hi": { "symbol": "say_hi", "returns": "void", "params": ["ptr"] }
    }
  }
}
```

```
import "greet";
greet.hi("ThaiThon");
```

No edits to the compiler's `lib_header.json` needed — `mymath`/`stdio`
still come from there, `greet` comes from your project's `lib.json`, and
both work together in the same program.