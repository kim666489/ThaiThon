# ThaiThon FFI — คู่มือฉบับละเอียด

เอกสารนี้อธิบายระบบ **FFI (Foreign Function Interface)** ของ ThaiThon แบบเจาะลึกถึงระดับ implementation จริงใน `src/include/Parser.hpp` และ `src/thaithon.cpp` ครอบคลุมทั้งสองทิศทาง:

- **Forward FFI** — ThaiThon เรียกฟังก์ชัน C/C++ ที่ compile แยกไว้
- **Reverse FFI** — โปรแกรม C/C++ เรียกฟังก์ชันที่เขียนด้วย ThaiThon กลับ

ทั้งหมดนี้ต่อยอดจาก syntax เดิมของภาษา (`import "name";` และ `name.func(args);`) โดย**ไม่ต้องแก้ lexer หรือ grammar ของภาษาเลย** — กลไกทั้งหมดอยู่ที่ชั้น parser + JSON registry (`lib_header.json` / project `lib.json`) + ขั้นตอนเรียก LLVM toolchain ใน `thaithon.cpp`

---

## สารบัญ

1. [ภาพรวมสถาปัตยกรรม](#ภาพรวมสถาปัตยกรรม)
2. [ไฟล์ในซอร์สที่เกี่ยวข้อง](#ไฟล์ในซอร์สที่เกี่ยวข้อง)
3. [Forward FFI: เรียก C/C++ จาก ThaiThon](#forward-ffi-เรียก-cc-จาก-thaithon)
4. [schema เต็มของ `lib_header.json`](#schema-เต็มของ-lib_headerjson)
5. [ขั้นตอนภายในตอน `import`](#ขั้นตอนภายในตอน-import)
6. [ขั้นตอนภายในตอนเรียกฟังก์ชัน (`callStatement`)](#ขั้นตอนภายในตอนเรียกฟังก์ชัน-callstatement)
7. [การแปลง argument เป็น LLVM operand (`lowerArgToLLVM`)](#การแปลง-argument-เป็น-llvm-operand-lowerargtollvm)
8. [โหมด backward-compatible: library ที่ไม่มี `params`](#โหมด-backward-compatible-library-ที่ไม่มี-params)
9. [ขั้นตอน build/link จริงใน `thaithon.cpp`](#ขั้นตอน-buildlink-จริงใน-thaithoncpp)
10. [Reverse FFI: เรียก ThaiThon จาก C/C++](#reverse-ffi-เรียก-thaithon-จาก-cc)
11. [Project-local `lib.json` และ JSON Merge Patch](#project-local-libjson-และ-json-merge-patch)
12. [ตัวอย่างครบวงจรจาก `test/`](#ตัวอย่างครบวงจรจาก-test)
13. [ข้อจำกัดและเหตุผลเชิง design](#ข้อจำกัดและเหตุผลเชิง-design)
14. [Checklist การ debug ปัญหา FFI](#checklist-การ-debug-ปัญหา-ffi)

---

## ภาพรวมสถาปัตยกรรม

```text
                    ┌─────────────────────┐
   .tt source  ───▶ │  Parser.hpp          │
                    │  - importStatement() │──▶ อ่าน lib_header.json / lib.json
                    │  - callStatement()   │──▶ ตรวจ "functions", "params", แปลงเป็น LLVM call
                    │  - lowerArgToLLVM()  │──▶ แปลง literal เป็น operand ตาม type ที่ประกาศ
                    └─────────┬────────────┘
                              │ เก็บ path ของ .c/.cpp ที่ต้อง link (externSources)
                              │ เก็บ system libs ที่ต้อง -l (externLibs)
                              ▼
                    ┌─────────────────────┐
   thaithon.cpp ───▶│  CompileIRToExecutable │
                    │  1) llc: .ll -> .o (โปรแกรมหลัก)          │
                    │  2) clang -c: แต่ละ externSource -> .o    │
                    │  3) clang: link ทุก .o + -l<libs> -> exe  │
                    └─────────────────────┘
```

หลักการสำคัญ: **compiler ไม่ได้ "รู้จัก" ฟังก์ชัน C/C++ ใด ๆ ล่วงหน้าเลย** ทุกอย่างมาจาก entry ใน `lib_header.json`/`lib.json` ที่ผู้ใช้ประกาศเอง — `declare` บอกลายเซ็นระดับ LLVM, `symbol` บอกชื่อจริงในไฟล์ object, `source` บอกไฟล์ที่ต้อง compile เพิ่ม

---

## ไฟล์ในซอร์สที่เกี่ยวข้อง

| ไฟล์ | หน้าที่ในระบบ FFI |
|---|---|
| `src/include/Parser.hpp` | `importStatement()` อ่าน `"source"`/`"libs"` จาก library entry, เก็บลง `externSources`/`externLibs`; `callStatement()` ตรวจ `"functions"` + `"params"` แล้ว emit LLVM `call`; `lowerArgToLLVM()` แปลง literal ให้ตรงกับ type ที่ประกาศไว้; `mergeProjectLibFile()` โหลดและ merge project-local `lib.json` |
| `src/thaithon.cpp` | `CompileExternSourceToObject()` เรียก `clang -c` compile ไฟล์ C/C++ แต่ละไฟล์เป็น `.o`; `LinkObjectsToExecutable()` เรียก `clang` เป็น linker driver รวม `.o` ทั้งหมด + `-l<lib>`; `CompileIRToExecutable()` เป็น orchestrator ของทั้งขั้นตอน |
| `lib_header.json` | registry ระดับ compiler ของทุก library ที่ import ได้ |
| `lib.json` (project-local) | registry เพิ่มเติมเฉพาะโปรเจกต์ ถูก merge เข้ากับ `lib_header.json` แบบ [RFC 7386](https://datatracker.ietf.org/doc/html/rfc7386) |

---

## Forward FFI: เรียก C/C++ จาก ThaiThon

### ขั้นตอนแบบทีละก้าว

**1) เขียนฟังก์ชัน C (หรือ C++ ที่ครอบด้วย `extern "C" { ... }`)**

```c
// mymath.c
#include <math.h>

int add_numbers(int a, int b) {
    return a + b;
}

double square_root(double x) {
    return sqrt(x);
}
```

> ถ้าเขียนเป็น C++ (`.cpp`) ต้องครอบฟังก์ชันด้วย `extern "C" { ... }` เพื่อกันไม่ให้ symbol ถูก C++ name-mangle เพราะ LLVM IR ที่ ThaiThon emit เรียก symbol แบบ C เปล่า ๆ (ตรงกับ field `"symbol"`)

**2) เพิ่ม entry ใน `lib_header.json`**

```json
"mymath": {
  "source": ["test/mymath.c"],
  "libs": ["m"],
  "declare": ["declare i32 @add_numbers(i32, i32)"],
  "functions": {
    "add": {
      "symbol": "add_numbers",
      "returns": "i32",
      "params": ["i32", "i32"]
    }
  }
}
```

**3) เรียกใช้จาก ThaiThon**

```thai
import "mymath";
let int sum = mymath.add(10, 20);
println(sum);
```

**4) build ด้วย `-target=exe` เพื่อให้เปิด linking**

```bash
./bin/thaithon.out -m=c -i=example.tt -target=exe
```

---

## schema เต็มของ `lib_header.json`

```json
{
  "<libraryName>": {
    "declare": ["<LLVM declare line>", "..."],
    "source": ["<path/to/file.c>", "..."],
    "libs": ["<system-lib-name>", "..."],
    "functions": {
      "<thaithonFunctionName>": {
        "symbol": "<real C symbol name>",
        "returns": "<LLVM return type>",
        "params": ["<LLVM type>", "..."]
      }
    }
  }
}
```

### คำอธิบายทีละ field (อ้างอิงจาก `importStatement()`/`callStatement()`)

| field | บังคับ/optional | ความหมาย |
|---|---|---|
| `declare` | บังคับ | LLVM `declare` statement ของฟังก์ชันในไลบรารีนั้น จะถูกเขียนลง IR ผ่าน `writer.importLibrary()` ตอน `import` |
| `source` | optional | รายการไฟล์ `.c`/`.cpp` ที่ต้อง compile แล้ว link เข้าไปด้วย path ถ้าเป็น relative จะ resolve กับ `ProjectPath` (โฟลเดอร์ของไฟล์ `.tt` ที่กำลัง compile) — ดูโค้ดจริงใน `importStatement()`: `if (srcPath.is_relative()) srcPath = (this->ProjectPath / srcPath).lexically_normal();` |
| `libs` | optional | ชื่อ system library ที่ต้อง link ด้วย `-l<name>` เช่น `"m"` (สำหรับ `<math.h>` อย่าง `sqrt`/`pow`, อยู่ใน libm ไม่ใช่ libc บน Linux/WSL) หรือ `"pthread"` |
| `functions.<name>.symbol` | บังคับ | ชื่อ symbol จริงของฟังก์ชันในไฟล์ object (ต้องตรงกับชื่อฟังก์ชันในซอร์ส C/C++ เป๊ะ ๆ) |
| `functions.<name>.returns` | optional (default `"i32"`) | ชนิด return แบบ LLVM ดูจากโค้ด: `string retType = funcInfo.value("returns", "i32");` |
| `functions.<name>.params` | optional | array ของ LLVM type ตามลำดับ argument ถ้า**ไม่มี field นี้เลย** ระบบจะ fallback ไปใช้โหมดเดิม (ดูหัวข้อ [backward-compatible](#โหมด-backward-compatible-library-ที่ไม่มี-params)) |

### ชนิด LLVM type ที่ `params`/`returns` รองรับ

รองรับเฉพาะ primitive เหล่านี้ (ตรวจใน `lowerArgToLLVM()`, ถ้าไม่ตรงจะ error ทันที):

- `ptr` — string (แทนด้วย pointer ไปยัง string literal ที่ intern ไว้)
- `i1` — bool
- `i8`, `i16`, `i32`, `i64` — integer ขนาดต่าง ๆ
- `float`, `double` — เลขทศนิยม

หากประกาศ type อื่นนอกเหนือจากนี้ compiler จะ error ทันทีด้วยข้อความ:

```text
'<funcName>': unsupported param type '<paramType>' declared in lib_header.json
(expected ptr / i1 / i8 / i16 / i32 / i64 / float / double)
```

---

## ขั้นตอนภายในตอน `import`

`importStatement()` ทำตามลำดับนี้เสมอเมื่อเจอ `import "name";`:

1. อ่าน string literal หลัง `import` แล้วเช็คว่า `lib_header` (JSON ที่โหลดไว้ตั้งแต่ construct parser) มี key นี้ — ถ้าไม่มีจะ error `Unknown library '<name>'`
2. เก็บชื่อ library ลง `this->import` (ใช้เช็คตอนเรียกฟังก์ชันภายหลังว่า import แล้วจริงหรือยัง)
3. เรียก `writer.importLibrary(name, lib_header)` เพื่อฝัง `"declare"` ทุกบรรทัดของ library นี้ลงใน LLVM IR ที่กำลังสร้าง
4. ถ้า library entry มี `"source"` → resolve path (relative กับ `ProjectPath`) แล้วเก็บลง `externSources` (deduplicate อัตโนมัติด้วย `find(...)`)
5. ถ้า library entry มี `"libs"` → เก็บชื่อ system lib ลง `externLibs` (deduplicate เช่นกัน)

`externSources`/`externLibs` นี้เองที่ `thaithon.cpp` จะอ่านออกไปหลัง parse เสร็จ (ผ่าน `parser.getExternSources()` / `parser.getExternLibs()`) เพื่อไปสั่ง compile/link ต่อ

---

## ขั้นตอนภายในตอนเรียกฟังก์ชัน (`callStatement`)

เมื่อ parser เจอ syntax แบบ `moduleName.functionName(args...)` จะทำตามลำดับ:

1. อ่านชื่อ module, ตรวจว่ามี `.` หรือ `::` คั่นจริง แล้วอ่านชื่อฟังก์ชัน
2. ตรวจว่า module นี้ถูก `import` มาก่อนหรือยัง (เช็คจาก `this->import`) — ถ้ายังไม่ import จะ error `Module '<name>' is not imported`
3. ตรวจว่า `lib_header[moduleName]["functions"][funcName]` มีอยู่จริง — ถ้าไม่มีจะ error `Unknown function '<funcName>' in module '<moduleName>'`
4. parse argument ในวงเล็บ (`parenStatement`) แล้ว split ด้วย comma (`splitArgs`)
5. อ่าน `symbol` และ `returns` (default `"i32"`) ของฟังก์ชันนั้นจาก JSON
6. **ถ้ามี `"params"`** → เข้าสู่โหมด typed call (ดูหัวข้อถัดไป): เช็ค argument count ให้ตรงกับ `params.size()` เป๊ะ ๆ ก่อน แล้วค่อยแปลงแต่ละ argument ด้วย `lowerArgToLLVM()`
7. **ถ้าไม่มี `"params"`** → fallback ไปโหมด backward-compatible (string argument เดี่ยว)
8. emit LLVM `call <returns> @<symbol>(<args>)` ลงใน IR

---

## การแปลง argument เป็น LLVM operand (`lowerArgToLLVM`)

ฟังก์ชันนี้รับ paramType (จาก `"params"`) และ token ของ argument ที่ parser อ่านมา แล้วแปลงเป็น LLVM operand string ที่ใส่ใน `call` ได้ทันที:

| paramType | argument ที่ยอมรับ | ผลลัพธ์ |
|---|---|---|
| `ptr` | string literal | intern string เป็น global constant แล้วคืน `ptr <label>` |
| `i1` | bool literal | `i1 0`/`i1 1` |
| `i8`/`i16`/`i32`/`i64` | integer literal | `<type> <value>` |
| `float`/`double` | integer หรือ float literal | แปลงเป็นทศนิยมเสมอ (integer literal จะถูกต่อ `.0` ให้อัตโนมัติ) |

argument ที่ไม่ใช่ literal ที่ compile-time ตีค่าได้ทันที (เช่นตัวแปรที่เพิ่ง assign มา) **ยังไม่รองรับ** ในเวอร์ชันนี้ (ดูหัวข้อข้อจำกัด)

---

## โหมด backward-compatible: library ที่ไม่มี `params`

ถ้า entry ของฟังก์ชันใน `lib_header.json` **ไม่มี field `"params"` เลย** (เช่น `stdio.print` ใน entry เริ่มต้นของ repo) `callStatement()` จะข้ามการตรวจ type ทั้งหมดแล้วใช้พฤติกรรมเดิม:

```json
"stdio": {
  "declare": ["declare i32 @puts(ptr)"],
  "functions": {
    "print": { "symbol": "puts", "returns": "i32" }
  }
}
```

- ต้องมี argument แรกเป็น string literal เท่านั้น (ไม่งั้น error `'<funcName>' expects a string as its first argument`)
- argument ที่เกินมาจะถูก**เพิกเฉย** พร้อม warning:

  ```text
  [Warning] print(): extra arguments ignored (only the string arg is used) at <line>:<col>
  -- add a "params" array to this function in lib_header.json to accept more/typed arguments
  ```

โหมดนี้มีไว้เพื่อให้ `lib_header.json` เวอร์ชันเก่า (ที่มีแค่ `stdio`) ยังใช้งานได้โดยไม่ต้องแก้ไฟล์เลยแม้ parser จะถูกอัปเกรดให้รองรับ typed params แล้วก็ตาม

---

## ขั้นตอน build/link จริงใน `thaithon.cpp`

เมื่อรัน `-target=exe` `CompileIRToExecutable()` จะทำตามลำดับนี้เสมอ:

1. เรียก `llc` แปลงไฟล์ `.ll` หลักของโปรแกรม ThaiThon เป็น `.o` (`mainObjPath`)
2. สำหรับ `externSources` แต่ละไฟล์ (ที่รวบรวมมาจากทุก `import` ในโปรแกรม): เรียก
   ```bash
   clang -c <source> -o <object>.o
   ```
   ไฟล์ `.o` ของ extern source จะถูกเขียนไว้ **ข้าง ๆ ไฟล์ `.ll` ตัวกลาง** ไม่ใช่ข้าง ๆ source เดิม
3. รวม object ทั้งหมด (main `.o` + extern `.o` ทุกไฟล์) แล้วเรียก
   ```bash
   clang <all objects> -l<lib1> -l<lib2> ... -o <output executable>
   ```
   โดย `-l<name>` มาจาก `externLibs` ที่สะสมไว้ตอน import ทุกครั้ง (เก็บใน `LinkObjectsToExecutable()`)
4. ลบไฟล์ `.o` ตัวกลางทั้งหมดทิ้งเสมอ (ไม่ลบ `.ll` ถ้าใช้ `-keep-ir=true`)

### ตัวเลือกที่เกี่ยวข้องกับ toolchain

| flag | ความหมาย |
|---|---|
| `-llc=<path>` | ระบุ path/ชื่อ binary ของ `llc` เอง (default: `llc` จาก PATH) |
| `-clang=<path>` | ระบุ path/ชื่อ binary ของ `clang` เอง (default: `clang` จาก PATH) |
| `-opt=<level>` | ส่ง IR ผ่าน `opt` ก่อน (`O0`–`O3`, `Os`, `Oz`) |
| `-opt-bin=<path>` | ระบุ path/ชื่อ binary ของ `opt` เอง (default: `opt`) |

### สิ่งที่เกิดขึ้นเมื่อ target ไม่ใช่ `exe`

ถ้าใช้ `-target=ir` หรือ `-target=asm` แต่โปรแกรมมี `import` ที่ประกาศ `"source"` ไว้ ระบบจะ**ไม่** compile/link extern source ให้ และจะพิมพ์ warning:

```text
[Warning] N extern C/C++ source file(s) were not linked because target != exe
```

---

## Reverse FFI: เรียก ThaiThon จาก C/C++

ฝั่งนี้ไม่ต้องพึ่ง `lib_header.json` เลย เพราะอาศัยข้อเท็จจริงที่ว่าฟังก์ชัน ThaiThon ถูก emit เป็น **LLVM function definition ปกติ** (`define <ret> @<funcName>(...)`) — ไม่ใช่ symbol แบบ C++ name-mangled จึงเรียกจาก C/C++ ได้ตรง ๆ ผ่าน `extern "C"`

### ขั้นตอน

**1) เขียนฟังก์ชัน ThaiThon ที่ต้องการ export**

```thai
function add(int a, int b) int {
    return a + b;
}

function greet(string name) void {
    println("Hello from ThaiThon: ");
    println(name);
}
```

**2) compile เป็น LLVM IR หรือ object code โดยตรง**

```bash
./bin/thaithon.out -m=c -i=export.tt -target=ir
llc -filetype=obj export.ll -o export.o
```

ฟังก์ชันจะถูก emit เป็น symbol ปกติ เช่น:

```llvm
define i32 @add(i32 %a, i32 %b) {
...
}
```

**3) ประกาศจาก C++ ด้วย `extern "C"` แล้วเรียกใช้**

ตัวอย่างจริงจาก `test/part7_reverse_ffi/caller.cpp`:

```cpp
extern "C" {
    int add(int a, int b);
    void greet(const char* name);
}

#include <iostream>

int main() {
    int total = add(10, 32);
    std::cout << "total=" << total << "\n";
    greet("ThaiThon");
    return total == 42 ? 0 : 1;
}
```

**4) link object ทั้งสองเข้าด้วยกัน**

```bash
g++ main.cpp export.o -o main
```

> ต้องมี LLVM toolchain จริง (`llc`, `clang`, หรือ `opt`) อยู่ใน environment ตอน build object/binary สุดท้าย

---

## Project-local `lib.json` และ JSON Merge Patch

นอกจากแก้ `lib_header.json` ของ compiler โดยตรง โปรเจกต์ผู้ใช้สามารถวาง `lib.json` ของตัวเองไว้ได้ทั้งสองที่:

- current working directory ที่รัน compiler อยู่, หรือ
- โฟลเดอร์เดียวกับไฟล์ `.tt` ที่ compile (`-i=...`)

`mergeProjectLibFile()` จะถูกเรียกสำหรับทั้งสองตำแหน่ง แล้วทำสองอย่าง:

1. `resolveLibSourcesInPlace()` — resolve `"source"` ทุกรายการใน `lib.json` ให้เป็น absolute path โดย relative กับ**โฟลเดอร์ของ `lib.json` เอง** (ไม่ใช่โฟลเดอร์ของ compiler)
2. `this->lib_header.merge_patch(projectLib)` — merge เข้ากับ `lib_header` ที่โหลดไว้แล้ว โดยใช้ [JSON Merge Patch (RFC 7386)](https://datatracker.ietf.org/doc/html/rfc7386) ผ่าน `nlohmann::json::merge_patch`

ความหมายของ merge patch ในบริบทนี้:

- library key ใหม่ทั้งหมด → ถูกเพิ่มเข้าไปตรง ๆ
- library key เดิมที่ซ้ำกับของ compiler → field ที่ระบุใหม่ใน `lib.json` จะทับ field เดิม, field ที่ไม่ได้พูดถึงยังคงอยู่
- ผลคือ `stdio`/`mymath` จาก `lib_header.json` ของ compiler และ `greet` จาก `lib.json` ของ project ใช้งานพร้อมกันได้ในโปรแกรมเดียว โดยไม่ต้องแตะไฟล์ของ compiler เลย

ตัวอย่างจากซอร์สจริง (`test/part3_local_lib/`):

```text
test/part3_local_lib/
├─ program.tt
└─ lib.json
```

`lib.json`:

```json
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

`program.tt`:

```thai
import "greet";
greet.hi("ThaiThon");
```

---

## ตัวอย่างครบวงจรจาก `test/`

| ไฟล์ | สิ่งที่สาธิต |
|---|---|
| `test/part1_basic/hello.tt` | `import "stdio"` + เรียกฟังก์ชันพื้นฐาน, target `ir` |
| `test/part3_local_lib/program.tt` + `lib.json` | project-local library แบบ typed params (`ptr`) |
| `test/part4_ffi/program.tt` | `mymath.add(10, 20)` — typed int FFI เต็มรูปแบบ, target `exe` |
| `test/part5_mixed/program.tt` | ผสม `stdio` (untyped) + `mymath` (typed) ในโปรแกรมเดียว |
| `test/part6_errors/bad_call.tt` | `mymath.add("abc", 2)` — ตั้งใจให้ error เพราะ argument type ไม่ตรงกับ `params` ที่ประกาศ (`i32`) |
| `test/part7_reverse_ffi/export.tt` + `caller.cpp` | reverse FFI แบบเต็ม พร้อมตัวอย่าง `extern "C"` ทั้ง function ที่ return ค่า และ function ที่รับ string |
| `test/mymath.c` | ตัวอย่างไฟล์ C ที่ compile ด้วย `clang -c` แล้ว link เข้า executable รวมถึงตัวอย่างการใช้ `<math.h>` ที่ต้องพึ่ง `"libs": ["m"]` |

---

## ข้อจำกัดและเหตุผลเชิง design

ทั้งหมดนี้เป็นข้อจำกัดที่ตั้งใจให้ scope เล็กไว้ก่อน ไม่ใช่ bug:

- **argument ต้องเป็น literal เท่านั้น** (`3`, `"hi"`, `2.5`) — ยังส่งตัวแปร ThaiThon เข้า FFI function ตรง ๆ ไม่ได้ เพราะ hand-rolled `Parser` นี้ยังไม่ track การ map ตัวแปร → LLVM register แบบเต็มรูปแบบ (ส่วนนั้นอยู่ใน normalizer pipeline แยกต่างหากคือ `Mono_c11.hpp`)
- **ชนิด param ที่รองรับมีจำกัด**: `ptr`, `i1/i8/i16/i32/i64`, `float`, `double` เท่านั้น — ยังไม่รองรับ struct หรือ pointer-to-struct
- **`-target=ir`/`-target=asm` ไม่ auto-link** extern source — ต้อง compile/link เองภายนอกถ้าต้องการ object/executable ที่สมบูรณ์
- **backward-compatible mode** (ไม่มี `"params"`) รองรับแค่ string argument เดียวเท่านั้น argument อื่นที่ตามมาจะถูกละเลยพร้อม warning ไม่ error

---

## Checklist การ debug ปัญหา FFI

| อาการ | สาเหตุที่เป็นไปได้ | วิธีตรวจสอบ |
|---|---|---|
| `Unknown library '<name>'` | ไม่มี key นี้ใน `lib_header.json` หรือ `lib.json` ที่ merge เข้ามา | ตรวจ spelling, ตรวจว่า `lib.json` วางถูกโฟลเดอร์ (cwd หรือโฟลเดอร์ของไฟล์ `.tt`) |
| `Module '<name>' is not imported` | เรียก `module.func()` โดยไม่มี `import "module";` ก่อนหน้า | เพิ่ม `import` statement ให้ครบก่อนเรียกใช้ |
| `Unknown function '<f>' in module '<m>'` | ชื่อฟังก์ชันไม่ตรงกับ key ใน `functions` ของ library entry | ตรวจ spelling ให้ตรงกับ JSON |
| `'<f>' expects N argument(s) but got M` | จำนวน argument ไม่ตรงกับความยาวของ `"params"` | นับ argument ให้ตรง หรือแก้ `"params"` ให้ถูกต้อง |
| `unsupported param type '<t>'` | ใส่ LLVM type ที่ไม่รองรับใน `"params"`/`"returns"` | ใช้เฉพาะ `ptr`/`i1`/`i8`/`i16`/`i32`/`i64`/`float`/`double` |
| `[Warning] extra arguments ignored` | เรียก untyped function (ไม่มี `"params"`) พร้อม argument เกินตัวเดียว | เพิ่ม `"params"` array ให้ฟังก์ชันนั้นถ้าต้องการรับ argument มากกว่า 1 ตัว |
| `[Warning] N extern C/C++ source file(s) were not linked` | ใช้ `-target=ir`/`-target=asm` ทั้งที่มี `"source"` ใน library ที่ import | เปลี่ยนไป `-target=exe` หรือ compile/link extern source เองภายนอก |
| `Clang not found in PATH` / `llc ... not found in PATH` | ไม่มี LLVM/Clang toolchain ติดตั้งหรือไม่ได้อยู่ใน PATH | ติดตั้ง LLVM/Clang, หรือระบุ path ตรง ๆ ด้วย `-clang=<path>` / `-llc=<path>` |
| linker บ่นหา symbol ของ `<math.h>` (เช่น `sqrt`) ไม่เจอ | ลืมใส่ `"libs": ["m"]` ใน library entry | เพิ่ม `"libs"` ให้ครบตาม system library ที่ฟังก์ชันนั้นต้องการ |