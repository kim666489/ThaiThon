# ThaiThon (ไทยธอน)

ThaiThon เป็นภาษาโปรแกรมมิ่งแบบ scripting / imperative ที่ผู้ใช้เขียนโค้ดในไฟล์นามสกุล `.tt` แล้ว compiler (เขียนด้วย C++) จะแปลงเป็น **LLVM IR** ก่อนส่งต่อให้ LLVM toolchain (`llc`, `clang`, `opt`) สร้างเป็น assembly หรือ executable จริงต่อไป ตัวโปรเจกต์ยังมาพร้อมระบบ **FFI** (เรียกโค้ด C/C++ จาก ThaiThon และในทางกลับกัน) และ **ModuleManager** ซึ่งเป็น package manager แบบ Git-based ของตัวเอง

เอกสารฉบับนี้เป็นการรวบรวมและเรียบเรียงเอกสารทั้งหมดในโปรเจกต์ (`README.md`, `README_FFI.md`, `README_ModuleManager.md`) ให้อยู่ในที่เดียว พร้อมอธิบายโครงสร้างไฟล์จริงในซอร์สโค้ด และตัวอย่างที่ดึงมาจากโฟลเดอร์ `test/` ของ repo โดยตรง

---

## สารบัญ

1. [ภาพรวมของ compiler pipeline](#ภาพรวมของ-compiler-pipeline)
2. [โครงสร้างโปรเจกต์](#โครงสร้างโปรเจกต์)
3. [สิ่งที่ต้องมีก่อน build](#สิ่งที่ต้องมีก่อน-build)
4. [การ build](#การ-build)
5. [การรัน compiler และตัวเลือกบรรทัดคำสั่ง](#การรัน-compiler-และตัวเลือกบรรทัดคำสั่ง)
6. [ไวยากรณ์ภาษา ThaiThon](#ไวยากรณ์ภาษา-thaithon)
7. [Built-in functions](#built-in-functions)
8. [ระบบ import และ library (`lib_header.json` / `lib.json`)](#ระบบ-import-และ-library)
9. [FFI: เรียก C/C++ จาก ThaiThon](#ffi-เรียก-cc-จาก-thaithon)
10. [Reverse FFI: เรียก ThaiThon จาก C/C++](#reverse-ffi-เรียก-thaithon-จาก-cc)
11. [คำสั่ง `link`: รวมหลายไฟล์ `.tt`](#คำสั่ง-link-รวมหลายไฟล์-tt)
12. [ModuleManager: package manager ของ ThaiThon](#modulemanager-package-manager-ของ-thaithon)
13. [ThaiThonRule.json: การตั้งค่า lexer/normalizer](#thaithonrulejson-การตั้งค่า-lexernormalizer)
14. [ชุดทดสอบ (`test/`) และ `makefile.test`](#ชุดทดสอบ-test-และ-makefiletest)
15. [ข้อจำกัดที่ควรรู้](#ข้อจำกัดที่ควรรู้)
16. [การแก้ปัญหาเบื้องต้น](#การแก้ปัญหาเบื้องต้น)
17. [License](#license)

---

## ภาพรวมของ compiler pipeline

```text
Source (.tt)
  -> Lexer            (ตัดคำตาม ThaiThonRule.json)
  -> Parser           (สร้าง AST / เก็บ import, link, function, class)
  -> LLVM IR (.ll)
     -> opt   (ถ้าระบุ -opt)     : ทำ optimization บน IR
     -> llc   (ถ้า target=asm)  : IR -> assembly
     -> clang (ถ้า target=exe)  : IR (+ C/C++ source ที่ link ผ่าน FFI) -> executable
```

ภาษาของ ThaiThon มีหน้าตาคล้าย JavaScript/C แบบง่าย ๆ และรองรับฟีเจอร์หลัก ๆ ดังนี้

- ตัวแปร `let` / `const` พร้อมระบุ type ได้ (หรือปล่อยให้อนุมานก็ได้)
- เงื่อนไข `if` / `else if` / `else`
- ลูป `while` พร้อม `continue` และ `pass`
- comment แบบ `//` และ `/* ... */`
- การประกาศฟังก์ชันแบบมี return type
- คลาส (field เท่านั้น ยังไม่มี method/inheritance)
- `import` สำหรับเรียก library (ทั้งจาก compiler และจาก project)
- `link` สำหรับผนวกไฟล์ ThaiThon อื่นเข้ามาตอน compile
- File I/O, JSON, string/array helper, print/input
- FFI สองทาง: ThaiThon เรียก C/C++ ผ่าน `lib_header.json`/`lib.json` และ C/C++ เรียกฟังก์ชัน ThaiThon กลับผ่าน `extern "C"`

---

## โครงสร้างโปรเจกต์

โครงสร้างไฟล์จริงในซอร์ส (ไม่รวมไฟล์ third-party ใน `src/include/nlohmann`, `src/include/asio`, `src/include/crow.h` ที่เป็น vendored libraries):

```text
ThaiThon/
├─ README.md                     # เอกสารฉบับนี้
├─ README_FFI.md                 # เอกสาร FFI แบบเจาะลึก (อ้างอิงในหัวข้อ FFI ด้านล่าง)
├─ README_ModuleManager.md       # เอกสาร ModuleManager แบบเจาะลึก
├─ lib_header.json               # registry ของ library ระดับ compiler (FFI)
├─ ThaiThonRule.json             # config ของ lexer/normalizer
├─ config.json                   # ค่า config เริ่มต้นของ compiler (เวอร์ชัน, flag เริ่มต้น)
├─ config/
│  └─ client.properties
├─ makefile                      # build/run compiler (C++) และ ModuleManager (Java)
├─ makefile.test                 # test runner สำหรับ test/part1 - part6
├─ src/
│  ├─ thaithon.cpp               # entry point ของ compiler (C++, parse args, orchestrate LLVM)
│  ├─ runtime.cpp                # runtime helper ฝั่ง C++
│  ├─ ModuleManager.java         # ตัว package manager (Java)
│  ├─ Json.java                  # JSON helper สำหรับ ModuleManager
│  └─ include/
│     ├─ Parser.hpp              # lexer + parser + code generation (LLVM IR)
│     ├─ ModuleWriter.hpp        # เขียนไฟล์ module/lib.json
│     ├─ Mono_c11.hpp            # normalizer pipeline
│     ├─ nlohmann/               # vendored JSON library (nlohmann/json)
│     ├─ asio/                   # vendored networking library
│     └─ crow.h                  # vendored micro web framework
├─ test/
│  ├─ mymath.c                   # ตัวอย่างไฟล์ C สำหรับ FFI
│  ├─ part1_basic/hello.tt
│  ├─ part2_stdio/hello.tt
│  ├─ part3_local_lib/           # program.tt + lib.json + program (binary ที่ build ไว้)
│  ├─ part4_ffi/                 # program.tt + program (binary)
│  ├─ part5_mixed/                # program.tt + program (binary)
│  ├─ part6_errors/bad_call.tt   # ตัวอย่างที่ตั้งใจให้ compile error
│  ├─ part7_reverse_ffi/         # export.tt + caller.cpp (reverse FFI)
│  └─ part8_module_manager/      # module_repo/ + project_demo/ สำหรับทดสอบ ModuleManager
├─ tmp_link/                     # ตัวอย่าง `link` แบบง่าย (main.tt + module.tt)
└─ bin/                          # ผลลัพธ์การ build (thaithon.out / thaithon.exe / ModuleManager .class)
```

> หมายเหตุ: `test/part3_local_lib/program`, `test/part4_ffi/program`, `test/part5_mixed/program` เป็น executable ที่ build ไว้แล้ว ถูก commit ไว้ใน repo เป็นตัวอย่างผลลัพธ์

---

## สิ่งที่ต้องมีก่อน build

| เครื่องมือ | ใช้ทำอะไร | จำเป็นสำหรับ |
|---|---|---|
| `g++` (รองรับ C++17) | build ตัว compiler หลัก (`thaithon.out`/`thaithon.exe`) | ทุก target |
| `clang` | compile IR/แหล่ง C/C++ ที่ link เป็น executable | `-target=exe` |
| `llc` | แปลง LLVM IR เป็น object/assembly | `-target=asm`, `-target=exe` |
| `opt` | ทำ optimization บน LLVM IR | เมื่อใช้ `-opt=...` |
| JDK (`javac`/`java`) | build/run `ModuleManager` | เฉพาะฟีเจอร์ package manager |
| `git` | ให้ `ModuleManager` clone module จาก remote repo | เฉพาะ `install`/`update` จาก `--repo=` |

ตรวจสอบว่า `clang`, `llc`, `opt` อยู่ใน `PATH` ก่อนใช้งานจริง (ดูหัวข้อ [การแก้ปัญหาเบื้องต้น](#การแก้ปัญหาเบื้องต้น))

---

## การ build

โปรเจกต์มี `makefile` หลักที่รวม target ไว้ทั้งฝั่ง C++ compiler และฝั่ง Java (`ModuleManager`)

### Linux / WSL / macOS

```bash
cd /path/to/ThaiThon
make build          # -> ./bin/thaithon.out
```

หรือสั่ง g++ ตรง ๆ (เทียบเท่ากับสิ่งที่ makefile เรียก):

```bash
g++ -o ./bin/thaithon.out -I ./src/include ./src/*.cpp -static-libgcc -static-libstdc++ -std=c++17
```

### Windows (PowerShell)

```powershell
make build_win       # -> ./bin/thaithon.exe
```

หรือ

```powershell
g++ -o .\bin\thaithon.exe -I .\src\include .\src\*.cpp -static-libgcc -static-libstdc++ -std=c++17
```

### Build ModuleManager (Java)

```bash
make build_tmi        # javac -d ./bin ./src/*.java
```

### Build ทุกอย่างพร้อมกัน

```bash
make build_all        # build + build_win + build_tmi
```

### รันผ่าน makefile โดยตรง

```bash
make run args="-m=c -i=main.tt -target=exe"      # เรียก ./bin/thaithon.out
make run_win args="-m=c -i=main.tt -target=exe"  # เรียก ./bin/thaithon.exe
make run_tmi args="install greet --repo=... --mode=project"  # เรียก ModuleManager
```

---

## การรันคอมไพเลอร์และตัวเลือกบรรทัดคำสั่ง

รูปแบบทั่วไป:

```bash
./bin/thaithon.out -m=<mode> -i=<input.tt> -target=<ir|asm|exe> [-o=<path>] [-opt=<level>] [-keep-ir=true]
```

### ตัวอย่าง

```bash
# compile เป็น LLVM IR (.ll)
./bin/thaithon.out -m=c -i=main.tt -target=ir

# compile เป็น assembly
./bin/thaithon.out -m=c -i=main.tt -target=asm

# compile เป็น executable
./bin/thaithon.out -m=c -i=main.tt -target=exe

# กำหนด output path เอง
./bin/thaithon.out -m=c -i=main.tt -target=exe -o=build/app

# เปิด optimization ระดับ O2 บน LLVM IR
./bin/thaithon.out -m=c -i=main.tt -target=exe -opt=O2

# เก็บไฟล์ .ll ที่สร้างระหว่าง build ไว้ (ไม่ลบทิ้ง)
./bin/thaithon.out -m=c -i=main.tt -target=exe -keep-ir=true
```

### รายละเอียดตัวเลือก (อ้างอิงตรงจาก `src/thaithon.cpp` และ `config.json`)

| flag | ชนิดค่า | ความหมาย |
|---|---|---|
| `-m=c` | string | โหมด compiler (ปัจจุบันรองรับค่า `c` เท่านั้น) |
| `-i=path` | string | path ของไฟล์ source `.tt` ที่จะ compile |
| `-target=ir\|asm\|exe` | string | รูปแบบ output ที่ต้องการ |
| `-o=path` | string | กำหนด output path เอง (ถ้าไม่ระบุจะใช้ค่า default ตามชื่อไฟล์ input) |
| `-opt=O0\|O1\|O2\|O3\|Os\|Oz` | string | ระดับ optimization ที่ส่งให้ `opt` (ค่าเริ่มต้นคือไม่ optimize) |
| `-keep-ir=true\|false` | bool | เก็บไฟล์ `.ll` ระหว่างขั้นตอน build ไว้หรือลบทิ้งหลัง build เสร็จ |

ค่า default ของบาง flag (`-m`, `-i`, `-o`, `version`, `debug`, `showToken`) ถูกกำหนดไว้ล่วงหน้าใน `config.json` ที่ root ของ compiler

---

## ไวยากรณ์ภาษา ThaiThon

### 1. Variables

```thai
let x = 10;
let int y = 20;
const string name = "ThaiThon";

x = x + 5;
```

- `let` : ตัวแปรที่สามารถเปลี่ยนค่าได้
- `const` : ตัวแปรคงที่ ไม่ให้ assign ใหม่
- สามารถระบุ type แบบชัดเจน (`let int y = 20;`) หรือปล่อยให้ compiler อนุมานก็ได้
- ประเภทที่รองรับ:
  - `int`
  - `float` / `double`
  - `string`
  - `bool`
  - `char`
  - `json`
  - ชื่อ class ที่ผู้ใช้กำหนดเอง

### 2. If / Else

```thai
if (x > 0) {
    print("positive");
} else if (x == 0) {
    print("zero");
} else {
    print("negative");
}
```

- เงื่อนไขใช้ตัวเปรียบเทียบเดี่ยว: `==`, `!=`, `<`, `>`, `<=`, `>=`
- เวอร์ชันนี้ยังไม่รองรับ `&&` / `||` แบบครบชุด

### 3. While

```thai
let i = 0;
while (i < 5) {
    println(i);
    i = i + 1;
}
```

#### 3.1 Continue / Pass

```thai
let i = 0;
while (i < 10) {
    i = i + 1;
    if (i == 5) {
        continue;
    }
    if (i == 8) {
        pass;
    }
    println(i);
}
```

- `continue;` กระโดดกลับไปเช็คเงื่อนไขของ loop ทันที
- `pass;` เป็น statement แบบ no-op ใช้เป็น placeholder หรือ "ไม่ทำอะไร"

### 4. Comment

```thai
// single-line comment
/*
  block comment
*/

let x = 1; // inline comment
```

comment จะถูก lexer/parser ข้ามก่อนทำการ dispatch statement เสมอ

### 5. Function

```thai
function add(int a, int b) int {
    return a + b;
}

function greet(string name) void {
    println("Hello, " + name);
}
```

- return type เขียนอยู่หลัง parameter list
- ฟังก์ชันต้องถูกประกาศไว้ก่อนถูกเรียกใช้ ในลำดับโค้ดเดียวกัน

### 6. Class

```thai
class Point {
    int x;
    int y;
}

let p = new Point();
p.x = 10;
p.y = 20;
```

- คลาสมีเฉพาะ field เท่านั้น (ยังไม่มี method, constructor, inheritance)
- สามารถส่ง instance เข้าฟังก์ชันได้

---

## Built-in functions

### Print / Input

```thai
print("Hello");
println("World");
let name = input("Your name: ");
```

### File I/O

```thai
let text = readfile("data.txt");
writefile("out.txt", "hello");
```

### String operations

```thai
let s = "abc";
let t = strconcat(s, "def");
println(t);
```

### Split / Array helpers

```thai
let arr = split("a,b,c", ",");
println(arraylen(arr));
println(arrayget(arr, 1));
```

### JSON

```thai
let j = jsonnew();
jsonsetstring(j, "name", "ThaiThon");
jsonsetint(j, "age", 5);
println(jsonstringify(j));
```

หรืออ่านจากไฟล์:

```thai
let doc = jsonreadfile("data.json");
println(jsongetstring(doc, "name"));
```

---

## ระบบ import และ library

### import แบบพื้นฐาน

```thai
import "stdio";
stdio.print("hello");
```

`import` จะอ่านจาก `lib_header.json` ที่อยู่ root ของ compiler หรือจาก `lib.json` ใน working directory / project directory (ดูรายละเอียดด้านล่าง)

### schema ของ `lib_header.json`

```json
{
  "stdio": {
    "declare": ["declare i32 @puts(ptr)"],
    "functions": {
      "print": {
        "symbol": "puts",
        "returns": "i32"
      }
    }
  },
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
}
```

คำอธิบายแต่ละ field:

- `declare` : LLVM `declare` statement ของฟังก์ชันนั้น
- `source` : ไฟล์ C/C++ ที่จะถูก compile แล้ว link ลง executable (relative กับโฟลเดอร์ของ `lib_header.json`/`lib.json` หรือใช้ absolute path ก็ได้)
- `libs` : *(optional)* system library ที่ต้อง link ด้วย `-l<name>` เช่น `"m"` สำหรับฟังก์ชันใน `<math.h>` อย่าง `sqrt`/`pow` (บน Linux/WSL อยู่ใน libm ไม่ใช่ libc) หรือ `"pthread"` สำหรับ POSIX threads
- `functions.<name>.symbol` : ชื่อ symbol จริงในไฟล์ C/C++ (ต้องตรงกับชื่อฟังก์ชันในซอร์ส)
- `functions.<name>.returns` : ชนิด return แบบ LLVM
- `functions.<name>.params` : LLVM type ของ argument ตามลำดับ เช่น `ptr` (string), `i1/i8/i16/i32/i64` (integer), `float`, `double`

### เรียกฟังก์ชันจาก library

```thai
import "mymath";
let int sum = mymath.add(10, 20);
println(sum);
```

รูปแบบการเรียกทั่วไปคือ `moduleName.functionName(args...)` และสามารถใช้เป็น expression ได้เช่นกัน:

```thai
let x = mymath.add(3, 4);
```

### Project-local `lib.json`

นอกจาก `lib_header.json` ของ compiler แล้ว โปรเจกต์ผู้ใช้สามารถมี `lib.json` ของตัวเองได้โดยตรง โดยไม่ต้องแก้ไฟล์ในโฟลเดอร์ install ของ compiler ทุกครั้ง:

```text
myproject/
├─ program.tt
├─ lib.json
├─ greet.c
```

`lib.json`:

```json
{
  "greet": {
    "source": ["greet.c"],
    "declare": ["declare void @say_hi(ptr)"],
    "functions": {
      "hi": {
        "symbol": "say_hi",
        "returns": "void",
        "params": ["ptr"]
      }
    }
  }
}
```

`program.tt`:

```thai
import "greet";
greet.hi("ThaiThon");
```

`lib.json` จะถูกค้นหาอัตโนมัติทั้งจาก **current working directory** ที่รัน compiler และจาก **โฟลเดอร์ที่เก็บไฟล์ `.tt`** ที่กำลัง compile (ผ่าน `-i=...`) โดยไม่ต้องใส่ flag เพิ่ม

compiler จะ **merge** `lib.json` เข้ากับ `lib_header.json` โดยใช้หลัก [JSON Merge Patch (RFC 7386)](https://datatracker.ietf.org/doc/html/rfc7386):

- library key ใหม่ทั้งหมด → ถูกเพิ่มเข้าไปตรง ๆ
- library key ที่มีอยู่แล้ว → merge แบบ field-by-field: field ที่ไม่ได้พูดถึงจะคงค่าเดิมไว้, field ที่ระบุใหม่ (เช่นเพิ่มฟังก์ชันใน `"functions"`) จะถูก merge เข้าไปโดยไม่กระทบ field อื่นของ library นั้น
- `"source"` ใน `lib.json` ของ project จะ resolve path แบบ relative กับ **โฟลเดอร์ของไฟล์ `lib.json` เอง** (ไม่ใช่โฟลเดอร์ของ compiler) ทำให้ไฟล์ `.c`/`.cpp` วางไว้ข้าง ๆ `lib.json` ได้เลย

ผลคือ `mymath`/`stdio` มาจาก `lib_header.json` ของ compiler ส่วน `greet` มาจาก `lib.json` ของ project และทั้งสองใช้งานร่วมกันได้ในโปรแกรมเดียวกัน โดยไม่ต้องแก้ไฟล์ของ compiler เลย

---

## FFI: เรียก C/C++ จาก ThaiThon

ฟีเจอร์นี้ทำให้เขียนฟังก์ชัน C/C++ จริง, compile แล้วเรียกจาก ThaiThon ได้ โดยไม่ต้องแก้ lexer/grammar ของภาษาเลย — ใช้ syntax `import "name";` และ `name.func(args);` ที่มีอยู่แล้วเป็นฐาน

### ไฟล์ที่เกี่ยวข้องในซอร์ส

- `src/include/Parser.hpp` — `importStatement()` อ่าน field `"source"` (optional) จาก `lib_header.json`/`lib.json` และจำ path ของไฟล์ C/C++ ที่ต้อง link; `callStatement()` รองรับ `"params"` ต่อฟังก์ชัน ทำให้ส่ง argument ชนิด int/float/string ที่ typed จริง ๆ ได้ ไม่ใช่แค่ string เดี่ยว
- `src/thaithon.cpp` — เมื่อ `-target=exe` ไฟล์ source ที่ link ทุกไฟล์จะถูก compile ด้วย `clang -c` แล้วนำ `.o` ที่ได้มา link รวมกับ `.o` ของโปรแกรม ThaiThon เป็น executable เดียว
- `lib_header.json` — มี key เสริม (optional) สองตัวต่อ library คือ `"source"` และ `"params"` (ดูตัวอย่าง entry `mymath`)

### ขั้นตอนการ link ฟังก์ชัน C/C++ ของตัวเอง

1. เขียนฟังก์ชันเป็น C (หรือ C++ ที่ครอบด้วย `extern "C" { ... }`):

   ```c
   // mymath.c
   int add_numbers(int a, int b) { return a + b; }
   ```

2. เพิ่ม entry ใน `lib_header.json` ต่อจาก library เดิม:

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

3. เรียกใช้จาก ThaiThon:

   ```thai
   import "mymath";
   mymath.add(3, 4);
   ```

4. build โดยเปิด linking:

   ```bash
   ./bin/thaithon.out -m=c -i=example.tt -target=exe
   ```

   ขั้นตอนนี้จะ compile `example.tt` เป็น IR, compile `mymath.c` เป็น object file แล้ว link ทั้งสองเข้าด้วยกันเป็น executable เดียว — `-target=ir`/`-target=asm` ยังใช้ได้ปกติแต่จะ **ไม่** auto-link source เพิ่ม (จะมี warning แจ้งเตือน)

### ข้อจำกัดของ FFI (ตั้งใจให้ scope เล็กไว้ก่อน)

- argument ที่ส่งเข้า linked function ต้องเป็น literal (`3`, `"hi"`, `2.5`) — การส่งตัวแปร ThaiThon เข้าไปตรง ๆ ยังไม่รองรับ เพราะ hand-rolled `Parser` นี้ยังไม่ track การ map ตัวแปร → register (ส่วนนั้นอยู่ใน normalizer pipeline แยกต่างหาก คือ `Mono_c11.hpp`)
- param type ที่รองรับ: `ptr`, `i1/i8/i16/i32/i64`, `float`, `double` — ยังไม่รองรับ struct หรือ pointer-to-struct
- ถ้า library entry ไม่มี `"params"` ระบบจะใช้พฤติกรรมเดิมแบบ single-string-argument (ทำให้ `lib_header.json` เดิมที่มีแค่ `stdio` ยังใช้งานได้โดยไม่ต้องแก้)

---

## Reverse FFI: เรียก ThaiThon จาก C/C++

compiler จะ emit ฟังก์ชัน ThaiThon ออกมาเป็น LLVM function definition แบบปกติ ทำให้โปรแกรม C/C++ สามารถ link เข้าหา symbol นั้นได้โดยตรง ตราบใดที่ประกาศด้วย ABI ที่ถูกต้อง

1. เขียนฟังก์ชัน ThaiThon:

   ```thai
   function add(int a, int b) int {
       return a + b;
   }
   ```

2. compile เป็น LLVM IR หรือ object code:

   ```bash
   ./bin/thaithon.out -m=c -i=export.tt -target=ir
   llc -filetype=obj export.ll -o export.o
   ```

   ฟังก์ชันจะถูก emit เป็น LLVM symbol ปกติ เช่น:

   ```llvm
   define i32 @add(i32 %a, i32 %b) {
   ...
   }
   ```

3. ประกาศจาก C++ ด้วย `extern "C"` แล้วเรียกใช้ (ตัวอย่างจริงจาก `test/part7_reverse_ffi/caller.cpp`):

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

4. link object ทั้งสองเข้าด้วยกัน:

   ```bash
   g++ main.cpp export.o -o main
   ```

การทำงานนี้เป็นไปได้เพราะชื่อฟังก์ชันของ ThaiThon ถูก emit เป็น plain LLVM/C symbol ไม่ใช่ชื่อแบบ C++ name-mangled — ถ้าฝั่งเรียกเป็น C++ ต้องครอบ prototype ด้วย `extern "C"` เพื่อกันการ mangle ชื่อ

> ต้องมี LLVM toolchain จริง (`llc`, `clang`, หรือ `opt`) อยู่ใน environment ตอน build object/binary สุดท้าย

---

## คำสั่ง `link`: รวมหลายไฟล์ `.tt`

ThaiThon รองรับคำสั่ง `link` เพื่อผนวกไฟล์ `.tt` อื่นเข้ามาตอน compile

```thai
import "stdio";
link "module.tt";

hello();
```

ไฟล์ `module.tt`:

```thai
function hello() void {
    println("Hello from module");
}
```

### หลักการ resolve path ของ `link`

`link "module.tt";` จะ resolve path แบบ relative กับ **directory ของไฟล์ `.tt` ที่กำลัง compile อยู่** ไม่ใช่ directory ของตัว compiler หรือ current working directory

โปรเจกต์แยก path ระหว่าง resource ของ compiler กับ resource ของ project ไว้ชัดเจน:

- **`MotherPath`** (compiler root) — ใช้หาไฟล์ที่เป็นของ compiler เอง เช่น `lib_header.json`, `ThaiThonRule.json`
- **`ProjectPath`** (project directory) — ใช้หาไฟล์ที่เป็นของโปรเจกต์ผู้ใช้ เช่น `link "module.tt"`, `lib.json` ใน project, และ source file ของ library ที่อยู่ในโปรเจกต์
- ถ้า path เป็น relative ใน project code จะ resolve จากโฟลเดอร์ของไฟล์ `.tt` ที่กำลัง compile
- ถ้า path เป็น relative ใน compiler-managed resource จะ resolve จาก root ของ compiler
- สำหรับ resource ที่เรียกจากฝั่ง project ให้ใช้ `exePath` เป็นฐานการค้นหาเมื่อจำเป็น เพื่อไม่ให้ scope ของ compiler resource กับ project resource ปนกัน

> สรุปสั้น: compiler resources → `MotherPath`, user project files → `ProjectPath`/source-file directory, ไม่ปนกัน

ตัวอย่างโครงสร้าง (ตรงกับ `tmp_link/` ในซอร์ส):

```text
tmp_link/
├─ main.tt      # import "stdio"; link "module.tt"; hello();
└─ module.tt    # function hello(): void { print("hi"); return; }
```

ถ้า `main.tt` อยู่ใน `tmp_link/` แล้วมี `link "module.tt";` มันจะหาไฟล์ที่ `tmp_link/module.tt` ให้อัตโนมัติ และชื่อไฟล์ที่ link ซ้ำกันจะถูก deduplicate ให้เองหากมีการ `link` ไฟล์เดียวกันซ้ำ

---

## ModuleManager: package manager ของ ThaiThon

`ModuleManager` (เขียนด้วย Java, ไฟล์ `src/ModuleManager.java` + `src/Json.java`) เป็นเครื่องมือจัดการ library/module ของ ThaiThon แบบสากล โดย:

- ติดตั้ง module จาก GitHub หรือ Git repository อื่น ๆ ผ่านคำสั่ง `git`
- เก็บ module ไว้ในโฟลเดอร์ `lib/` ของ compiler (permanent mode) หรือของ project (project mode)
- ตรวจ version ของ module และความเข้ากันได้กับ compiler ปัจจุบัน
- รองรับคำสั่ง remove / uninstall / update / search / list / link / help

### โครงสร้างที่เกี่ยวข้อง

```text
ThaiThon/
├─ lib/                     # module ของ compiler (permanent mode)
├─ lib_header.json
├─ config.json               # เก็บ "version" ของ compiler ปัจจุบัน
├─ src/
│  ├─ ModuleManager.java
│  └─ Json.java
├─ test/
│  └─ part8_module_manager/
│     ├─ module_repo/        # ตัวอย่าง module repo (มี lib.json + greet.c)
│     └─ project_demo/       # ตัวอย่างโปรเจกต์ที่ใช้ module ผ่าน lib.json
└─ README_ModuleManager.md
```

### โหมดการติดตั้ง

**1. permanent mode** — ติดตั้งไว้ใน compiler root:

```bash
java -cp ./bin ModuleManager install greet --repo=https://github.com/owner/thaithon-greet.git --mode=permanent
```

ผลลัพธ์: `<compiler_root>/lib/greet/`

**2. project mode** — ติดตั้งสำหรับ project ปัจจุบัน และเขียนลง `lib.json` ของ project:

```bash
java -cp ./bin ModuleManager install greet --repo=https://github.com/owner/thaithon-greet.git --mode=project
```

ผลลัพธ์: `<project_root>/lib/greet/` และ `<project_root>/lib.json`

### `lib.json` ที่ module repo ต้องมี

```json
{
  "greet": {
    "name": "greet",
    "version": "1.0.0",
    "compilerVersion": "0.0.1",
    "source": ["greet.c"],
    "declare": [
      "declare void @say_hi(ptr)"
    ],
    "functions": {
      "hi": {
        "symbol": "say_hi",
        "returns": "void",
        "params": ["ptr"]
      }
    }
  }
}
```

### การตรวจสอบเวอร์ชัน

`ModuleManager` เปรียบเทียบ `compilerVersion` ที่ module ต้องการกับค่า `"version"` ใน `config.json` ของ compiler:

- ถ้า compiler version ปัจจุบัน >= `compilerVersion` ที่ module ต้องการ → ติดตั้งผ่าน
- ถ้า module ต้องการ compiler version สูงกว่าที่มีอยู่ → หยุดทำงานทันทีพร้อม error message

ตัวอย่างเช่น module ระบุ `"compilerVersion": "1.0.0"` แต่ `config.json` ของเครื่องมี `"version": "0.0.1"` → ติดตั้งไม่ผ่าน

### คำสั่งอื่น ๆ

```bash
# ค้นหา module
java -cp ./bin ModuleManager search greet

# อัปเดต module ไปยังเวอร์ชันที่ระบุ
java -cp ./bin ModuleManager update greet --version=1.1.0

# ลบ module (ใช้ได้ทั้งสอง alias)
java -cp ./bin ModuleManager uninstall greet
java -cp ./bin ModuleManager remove greet

# เชื่อม module ที่ติดตั้งแล้วเข้ากับ project ปัจจุบัน
java -cp ./bin ModuleManager link greet

# แสดงรายการ module ที่ติดตั้งแล้ว
java -cp ./bin ModuleManager list

# แสดงวิธีใช้งาน
java -cp ./bin ModuleManager help
```

### ตัวอย่าง project demo ในซอร์ส

`test/part8_module_manager/project_demo/main.tt`:

```thai
import "greet";

greet.hi("ThaiThon");
```

`test/part8_module_manager/project_demo/lib.json`:

```json
{
  "greet": {
    "path": "../lib/greet",
    "version": "1.0.0",
    "compilerVersion": "0.0.1",
    "mode": "project"
  }
}
```

`test/part8_module_manager/module_repo/lib.json` คือตัวอย่าง module repo ที่ `ModuleManager` จะ `git clone` แล้วคัดลอกลง `lib/` ให้ทั้งฝั่ง compiler หรือฝั่ง project

### ทดสอบด้วย local repo (ไม่ต้องพึ่ง remote จริง)

```bash
java -cp ./bin ModuleManager install greet --repo=/path/to/ThaiThon/test/part8_module_manager/module_repo --mode=project
```

---

## ThaiThonRule.json: การตั้งค่า lexer/normalizer

`ThaiThonRule.json` ที่ root ของ compiler เป็นไฟล์ config ที่กำหนดพฤติกรรมของ lexer และ normalizer โดยไม่ต้องแก้โค้ด C++ โดยตรงในหลาย ๆ กรณี โครงสร้างหลัก ๆ มีดังนี้:

- **`_config`** — ค่าพื้นฐาน เช่น `enable_char_type`, `keep_newline`, `line_terminator` (ปัจจุบันคือ `;`), `stmt_separator`, `tab_size`
- **`_error_policy`** — กำหนดว่าแต่ละประเภท error (`invalid_char`, `unclosed_string`, `unknown_token`, ฯลฯ) ให้ `exit` โปรแกรมทันที หรือแค่ `warn`
- **`lexer.keyword`** — ตาราง keyword ทั้งหมดของภาษา (`import`, `link`, `let`, `const`, `if`, `else`, `while`, `function`/`fn`, `class`, `return`, `print`, `println`, `continue`, `pass`)
- **`lexer.operator`** — mapping ของ operator เป็น token type เช่น `==` → `EQ`, `!=` → `NEQ`
- **`lexer.symbol`** — mapping ของ symbol เดี่ยว เช่น `(` → `LPAREN`, `{` → `LBRACE`, `::` → `SCOPE`
- **`normalizer.collect_brackets` / `collect_delimiters` / `collect_expr_mode` / `collect_types`** — กำหนดว่า normalizer จะ "เก็บ" กลุ่ม token แบบไหนเป็น expression, list, map, function-call, หรือ code-block ตาม bracket และ delimiter ที่พบ

ไฟล์นี้เหมาะสำหรับผู้ที่ต้องการปรับ/ต่อยอด grammar ระดับ token โดยไม่ต้องแก้ `Parser.hpp` โดยตรงในหลายกรณี (เช่น เพิ่ม keyword ใหม่)

---

## ชุดทดสอบ (`test/`) และ `makefile.test`

`makefile.test` เป็น test runner ที่ครอบคลุม part 1–6 (ส่วน part 7 และ part 8 ต้องรันแยกตามขั้นตอนของ FFI/ModuleManager ด้านบน):

```bash
make -f makefile.test build   # build compiler ก่อน
make -f makefile.test part1   # import + print พื้นฐาน (target=ir)
make -f makefile.test part2   # stdio library (target=ir)
make -f makefile.test part3   # local lib ผ่าน project lib.json (target=exe)
make -f makefile.test part4   # FFI เรียก mymath.add (target=exe)
make -f makefile.test part5   # ผสม stdio + mymath ในไฟล์เดียว (target=exe)
make -f makefile.test part6   # ตัวอย่างที่ตั้งใจให้ error เพราะ type ไม่ตรง (target=ir)
make -f makefile.test all     # รันทุก part ตามลำดับ
```

สรุปแต่ละ part:

| Part | โฟลเดอร์ | สิ่งที่ทดสอบ |
|---|---|---|
| 1 | `test/part1_basic/` | `import "stdio"; stdio.print(...)` พื้นฐาน, target `ir` |
| 2 | `test/part2_stdio/` | ตัวอย่าง stdio library เพิ่มเติม, target `ir` |
| 3 | `test/part3_local_lib/` | project-local `lib.json` + `greet.c`, target `exe` |
| 4 | `test/part4_ffi/` | เรียก `mymath.add(10, 20)` ผ่าน FFI, target `exe` |
| 5 | `test/part5_mixed/` | ใช้ทั้ง `stdio` และ `mymath` ในไฟล์เดียว, target `exe` |
| 6 | `test/part6_errors/` | `mymath.add("abc", 2)` — คาดหวังให้ compile error เพราะ argument ผิด type |
| 7 | `test/part7_reverse_ffi/` | reverse FFI: `export.tt` ถูกเรียกจาก `caller.cpp` |
| 8 | `test/part8_module_manager/` | ทดสอบ `ModuleManager` ทั้ง module repo และ project demo |

---

## ข้อจำกัดที่ควรรู้

- `&&` / `||` ยังไม่รองรับแบบเต็ม
- argument ที่ส่งเข้า FFI function ยังจำกัดให้เป็น literal หรือ operand ที่ compile-time ตีค่าได้เท่านั้น (ยังส่งตัวแปรตรง ๆ ไม่ได้)
- `class` ยังไม่มี method หรือ inheritance — มีแค่ field
- ไม่มี garbage collector หรือระบบจัดการหน่วยความจำแบบเต็มรูปแบบ
- `-target=ir` และ `-target=asm` จะไม่ auto-link ไฟล์ source C/C++ เหมือนกับ `-target=exe`
- `string` และ `json` ในระดับ LLVM IR มักถูกแทนด้วย pointer จึงต้องใช้ helper function เช่น `strconcat`, `jsonset*`, `jsonget*` แทนการดำเนินการตรง ๆ

---

## การแก้ปัญหาเบื้องต้น

### Error: Linked file not found

```text
[Error] Linked file not found: /path/to/project/module.tt
```

สาเหตุส่วนใหญ่คือ path resolution ชี้ผิดโฟลเดอร์ แก้ไขโดย:

- ตรวจว่า `link` ใช้ path แบบ relative กับโฟลเดอร์ของไฟล์ `.tt` ที่กำลัง compile จริง
- normalize path ก่อนเปิดไฟล์ (เช่นใช้ `lexically_normal`)
- ตรวจว่า `module.tt` อยู่ในโฟลเดอร์เดียวกับ `main.tt` ตามที่ตั้งใจไว้

### Error: cannot open input file

```text
[Error] Cannot open input file: main.tt
```

ตรวจว่า path ที่ส่งเข้า `-i=...` ถูกต้อง เช่น `-i=tmp_link/main.tt` (Linux/macOS) หรือ `-i=tmp_link\main.tt` (Windows)

### Error: LLVM tools not found

ตรวจว่า `clang`, `llc`, `opt` อยู่ใน `PATH`:

```bash
which clang
which llc
which opt
```

หรือบน Windows PowerShell:

```powershell
Get-Command clang
Get-Command llc
Get-Command opt
```

---

## License

ในซอร์สโค้ดปัจจุบันยังไม่มีไฟล์ license (เช่น `LICENSE`/`LICENSE.md`) แนบมาด้วย หากต้องการเผยแพร่หรือใช้งานต่อในเชิงพาณิชย์ ควรติดต่อผู้ดูแล repo เพื่อยืนยันเงื่อนไขการใช้งานก่อน

---

## สรุป

ThaiThon เป็นภาษาขนาดเล็กที่ออกแบบมาให้:

- คอมไพล์เป็น LLVM IR แล้วต่อยอดเป็น asm/executable ได้จริง
- เรียก library C/C++ ได้ตรง ๆ ผ่านระบบ FFI สองทาง
- ใช้ `link` เชื่อมไฟล์ ThaiThon หลายไฟล์เข้าด้วยกันได้
- มี `lib.json` / `lib_header.json` สำหรับจัดการ library ทั้งระดับ compiler และระดับ project
- มี `ModuleManager` เป็น package manager ของตัวเองแบบ Git-based

หากต้องการเริ่มทำโปรเจกต์ต่อ แนะนำลำดับดังนี้:

1. สร้างไฟล์ `.tt`
2. `import` library ที่ต้องการ (จาก `lib_header.json` หรือ `lib.json` ของ project)
3. ใช้ `link` เพื่อแยกไฟล์ย่อยตามต้องการ
4. Compile ด้วย `-target=exe`
5. ถ้าต้องขยายระบบ ให้เขียน C/C++ FFI เพิ่ม หรือใช้ `ModuleManager` เพื่อดึง module จาก Git repository อื่น