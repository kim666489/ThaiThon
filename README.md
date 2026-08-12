# ThaiThon

ThaiThon เป็นภาษาแบบ scripting / imperative ที่คอมไพล์เป็น LLVM IR แล้วสั่งให้ LLVM toolchain สร้าง assembly หรือ executable ต่อไปได้ เอกสารนี้ครอบคลุมหลักการใช้งาน การ build/run, `link`, FFI, และตัวอย่างใช้งานแบบละเอียด

## ภาพรวม

ThaiThon รับ source code ที่มีนามสกุล .tt แล้วผ่านกระบวนการดังนี้

```text
Source (.tt)
  -> Lexer
  -> Parser
  -> LLVM IR (.ll)
     -> opt (optional)
     -> llc (for asm/object)
     -> clang (for executable)
```

ภาษาของ ThaiThon มีความคล้าย JavaScript/C-like แบบง่าย ๆ และมีฟีเจอร์พื้นฐาน เช่น

- ตัวแปร `let` / `const`
- `if` / `else` / `while`
- `continue` และ `pass`
- comment แบบ `//` และ `/* ... */`
- ฟังก์ชัน
- คลาสและ field
- `import` เพื่อเรียก library
- `link` เพื่อผนวกไฟล์ ThaiThon อื่น ๆ
- file I/O, JSON, split, print/input
- FFI เชื่อมกับ C/C++ ผ่าน `lib_header.json` หรือ `lib.json`
- reverse FFI: C/C++ สามารถเรียกฟังก์ชันที่ ThaiThon emit ออกมาได้ด้วย `extern "C"`

## โครงสร้างโปรเจกต์

```text
ThaiThon/
├─ README.md
├─ README_FFI.md
├─ lib_header.json
├─ ThaiThonRule.json
├─ makefile
├─ src/
│  ├─ thaithon.cpp
│  └─ include/
│     ├─ Parser.hpp
│     ├─ ModuleWriter.hpp
│     ├─ Executor.hpp
│     ├─ Mono_c11.hpp
│     └─ ...
├─ test/
│  ├─ part1_basic/
│  ├─ part2_stdio/
│  ├─ part3_local_lib/
│  ├─ part4_ffi/
│  ├─ part5_mixed/
│  └─ part6_errors/
├─ bin/
└─ ...
```

## การ build

### Linux / WSL / macOS

```bash
cd /path/to/ThaiThon
make build
```

หรือ build แบบตรง ๆ

```bash
g++ -o ./bin/thaithon.out -I ./src/include ./src/*.cpp -static-libgcc -static-libstdc++ -std=c++17
```

### Windows PowerShell

```powershell
d:
cd D:\Project\part3\ThaiThon
make build
```

หรือใช้ command แบบตรง

```powershell
g++ -o .\bin\thaithon.exe -I .\src\include .\src\*.cpp -static-libgcc -static-libstdc++ -std=c++17
```

> ต้องมี LLVM tools อยู่ใน PATH ได้แก่ `llc`, `clang`, และ `opt` (ถ้าจะใช้ `-opt`)

## การรัน compiler

### ตัวอย่างสำหรับ target แบบ IR

```bash
./bin/thaithon.out -m=c -i=main.tt -target=ir
```

### ตัวอย่างสำหรับ target แบบ assembly

```bash
./bin/thaithon.out -m=c -i=main.tt -target=asm
```

### ตัวอย่างสำหรับ target แบบ executable

```bash
./bin/thaithon.out -m=c -i=main.tt -target=exe
```

### ตัวเลือกเพิ่มเติม

```bash
./bin/thaithon.out -m=c -i=main.tt -target=exe -o=build/app
./bin/thaithon.out -m=c -i=main.tt -target=exe -opt=O2
./bin/thaithon.out -m=c -i=main.tt -target=exe -keep-ir=true
```

คำอธิบายตัวเลือก:

- `-m=c` : โหมด compiler (ปัจจุบันใช้ `c` เท่านั้น)
- `-i=path` : ไฟล์ source .tt ที่จะคอมไพล์
- `-target=ir|asm|exe` : รูปแบบ output
- `-o=path` : กำหนด output path
- `-opt=O0|O1|O2|O3|Os|Oz` : เปิด optimization บน LLVM IR
- `-keep-ir=true` : เก็บไฟล์ .ll ระหว่างขั้นตอน build ไว้

## ไวยากรณ์พื้นฐาน

### 1. Variables

```thai
let x = 10;
let int y = 20;
const string name = "ThaiThon";

x = x + 5;
```

- `let` : ตัวแปรที่สามารถเปลี่ยนค่าได้
- `const` : ตัวแปรคงที่ ไม่ให้ assign ใหม่
- สามารถเขียน type แบบชัดเจนหรือไม่ชัดเจนได้
- ประเภทที่รองรับมีดังนี้
  - `int`
  - `float` / `double`
  - `string`
  - `bool`
  - `char`
  - `json`
  - ชื่อ class ที่กำหนดเอง

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

- เงื่อนไขใช้การเปรียบเทียบแบบเดียว เช่น `==`, `!=`, `<`, `>`, `<=`, `>=`
- ยังไม่มี `&&` / `||` แบบครบชุดในเวอร์ชันนี้

### 3. While

```thai
let i = 0;
while (i < 5) {
    println(i);
    i = i + 1;
}
```

### 3.1. Continue / Pass

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

- `continue;` จะ jump กลับไปยัง loop condition
- `pass;` เป็น statement no-op ที่ใช้เป็น placeholder หรือ “do nothing”

### 4. Comment

```thai
// single-line comment
/*
  block comment
*/

let x = 1; // inline comment
```

- รองรับคำสั่ง comment แบบ `// ...`
- รองรับ block comment แบบ `/* ... */`
- comment จะถูก lexer/statement parser ข้ามก่อนทำ statement dispatch

### 5. Function

```thai
function add(int a, int b) int {
    return a + b;
}

function greet(string name) void {
    println("Hello, " + name);
}
```

- รูปแบบ return type อยู่หลัง parameter list
- ตัวอย่างด้านบนแสดง `function` ที่ต้องมี `return` ตามลำดับ
- ฟังก์ชันต้องถูกประกาศก่อนใช้งานในลำดับโค้ดเดียวกัน

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

- คลาสมี field อย่างเดียว
- ไม่มี inheritance, method, constructor แบบครบชุด
- สามารถส่ง instance เข้า function ได้

## การใช้ import และ library

### import แบบพื้นฐาน

```thai
import "stdio";
print("hello");
```

`import` จะ read จาก `lib_header.json` ที่อยู่ใน root ของ compiler หรือใน working directory / project directory ตามความเหมาะสม

### FFI library schema

`lib_header.json` เป็น registry สำหรับ library ที่ใช้ FFI

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

คำอธิบาย:

- `declare` : LLVM declare statement
- `source` : ไฟล์ C/C++ ที่จะ compile แล้ว link ลง executable
- `libs` : system library ที่ต้อง link เช่น `m`, `pthread`
- `functions` : map ชื่อฟังก์ชันภายใน ThaiThon กับ symbol จริงใน C/C++
- `params` : ประเภทของ argument แบบ LLVM เช่น `i32`, `double`, `ptr`

### เรียกฟังก์ชันจาก library

```thai
import "mymath";
let int sum = mymath.add(10, 20);
println(sum);
```

### Reverse FFI: C/C++ เรียก ThaiThon function

ThaiThon compile แล้ว emit ฟังก์ชันเป็น LLVM symbol ปกติ จึงสามารถถูก C/C++ เรียกผ่าน `extern "C"` ได้ ดังนี้

```thai
function add(int a, int b) int {
    return a + b;
}
```

```cpp
extern "C" int add(int a, int b);

int main() {
    int total = add(10, 32);
    return total == 42 ? 0 : 1;
}
```

> เมื่อใช้ C++ ต้องมี `extern "C"` เพื่อป้องกัน symbol name mangling

รูปแบบการเรียก:

```thai
moduleName.functionName(args...);
```

และสามารถใช้เป็น expression ได้ด้วย

```thai
let x = mymath.add(3, 4);
```

## การ link ไฟล์ ThaiThon หลายไฟล์

ThaiThon รองรับคำสั่ง `link` เพื่อผนวกไฟล์ `.tt` อื่นเข้ามาในตอน compile

```thai
import "stdio";
link "module.tt";

hello();
```

ไฟล์ `module.tt`

```thai
function hello() void {
    println("Hello from module");
}
```

### หลักการ resolve path ของ `link`

`link "module.tt";` จะ resolve relative กับ directory ของไฟล์ `.tt` ที่กำลัง compile อยู่ ไม่ใช่ directory ของตัว compiler หรือ current working directory แบบเดิม

นอกจากนี้ยังมีการแยก path ระหว่าง resource ของ compiler กับ project ของผู้ใช้ไว้ชัดเจนดังนี้:

- `MotherPath` / compiler root : ใช้สำหรับหาไฟล์ที่เป็นของ compiler เอง เช่น `lib_header.json`, `ThaiThonRule.json`
- `ProjectPath` / project directory : ใช้สำหรับหาไฟล์ที่เป็นของโปรเจกต์ผู้ใช้ เช่น `link "module.tt"`, `lib.json` ใน project, และ source file จาก library ที่อยู่ใน project
- ถ้า path เป็น relative ใน project code จะ resolve จาก folder ของไฟล์ `.tt` ที่กำลัง compile อยู่
- ถ้า path เป็น relative ใน compiler-managed resource จะ resolve จาก root ของ compiler
- สำหรับ resource ที่เรียกจาก project ของผู้ใช้ ให้ใช้ `exePath` เป็นฐานการค้นหาหากจำเป็น เช่น กรณี runtime หรือ compiler engine ควรใช้ project scope ให้ถูกต้อง แยกออกจาก compiler resource

> สรุปสั้น: compiler resources -> `MotherPath`, user project files -> `ProjectPath`/source-file directory, ไม่ให้ปนกัน

ตัวอย่างโครงสร้าง:

```text
project/
├─ main.tt
├─ module.tt
└─ lib.json
```

ถ้า `main.tt` อยู่ใน `project/` แล้วใช้

```thai
link "module.tt";
```

มันจะหาที่ `project/module.tt` โดยอัตโนมัติ

### ตัวอย่างการใช้งานจริง

```thai
import "stdio";
link "extra/math.tt";

let x = add(3, 4);
println(x);
```

> ชื่อ file ที่ link จะถูก deduplicate อัตโนมัติ ถ้ารวมซ้ำกันแล้วจะไม่โหลดซ้ำ

## Built-in functions

ThaiThon มี built-ins สำหรับงานพื้นฐานที่อยู่ใน runtime

### Print / input

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

### Split / array helpers

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

หรืออ่านจากไฟล์

```thai
let doc = jsonreadfile("data.json");
println(jsongetstring(doc, "name"));
```

## Project-local lib.json

นอกจาก `lib_header.json` ของ compiler แล้ว ยังสามารถปล่อย `lib.json` ไว้ใน project ของตัวเองได้โดยตรง

```text
myproject/
├─ program.tt
├─ lib.json
├─ greet.c
```

`lib.json`

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

`program.tt`

```thai
import "greet";
greet.hi("ThaiThon");
```

> โปรแกรมจะ auto merge `lib.json` กับ `lib_header.json` โดยไม่ต้องแก้ compiler root

## ตัวอย่างโปรเจกต์ใน repo

### ตัวอย่างพื้นฐาน

```thai
import "stdio";
println("Hello ThaiThon");
```

### ตัวอย่าง local library

```thai
import "greet";
greet.hi("ThaiThon");
```

### ตัวอย่าง FFI

```thai
import "mymath";
let int total = mymath.add(10, 20);
println(total);
```

### ตัวอย่าง errors

```thai
import "mymath";
mymath.add("abc", 2);
```

โค้ดนี้คาดว่าจะ error เพราะ parameter type ไม่ตรง

## ตัวอย่าง command line จริง ๆ

### Compile เป็น IR

```bash
./bin/thaithon.out -m=c -i=example.tt -target=ir
```

### Compile เป็น executable

```bash
./bin/thaithon.out -m=c -i=example.tt -target=exe
```

### Compile พร้อม link file อื่น

```bash
./bin/thaithon.out -m=c -i=project/main.tt -target=exe
```

## ข้อจำกัดที่ควรรู้

- `&&` / `||` ยังไม่รองรับแบบเต็ม
- การเรียก FFI function ยังจำกัดให้ pass literal หรือ operand ที่ compile-time สามารถตีค่าได้
- `class` ยังไม่มี method หรือ inheritance
- ไม่มี garbage collector หรือ memory management แบบเต็ม
- `-target=ir` และ `-target=asm` จะไม่ auto-link C/C++ source files ตามตัวอย่างเดียวกับ `-target=exe`
- `string` และ `json` ในระดับ IR มักถูกแปลงเป็น pointer จึงต้องใช้ helper functions อย่าง `strconcat`, `jsonset*`, `jsonget*`

## การแก้ปัญหาเบื้องต้น

### Error: Linked file not found

```text
[Error] Linked file not found: /path/to/project/module.tt
```

สาเหตุส่วนใหญ่คือ path resolution ใช้ผิดโฟลเดอร์

แก้ไขได้โดย:

- ให้ `link` resolve relative กับ directory ของไฟล์ `.tt` ที่กำลังคอมไพล์
- ใช้ `lexically_normal` หรือ normalize path ก่อนเปิดไฟล์
- ตรวจสอบว่า `module.tt` อยู่ใน directory เดียวกับ `main.tt`

### Error: cannot open input file

```text
[Error] Cannot open input file: main.tt
```

ตรวจดูว่าพาธที่ส่งเข้ามาถูกต้องหรือยัง เช่น `-i=tmp_link/main.tt` หรือ `-i=tmp_link\main.tt`

### Error: LLVM tools not found

ตรวจว่า `clang`, `llc`, `opt` อยู่ใน PATH แล้ว

```bash
which clang
which llc
which opt
```

หรือบน Windows

```powershell
Get-Command clang
Get-Command llc
Get-Command opt
```

## สรุป

ThaiThon เป็นภาษาแบบง่าย ๆ ที่ออกแบบให้

- คอมไพล์เป็น LLVM IR
- เรียก library C/C++ ได้ตรง ๆ
- ใช้ `link` เชื่อมไฟล์ ThaiThon หลายไฟล์ได้
- ใช้ `lib.json` / `lib_header.json` สำหรับ management library
- เหมาะกับโครงการภาษาเล็ก ๆ ที่ต้องการโครงสร้างแบบประกาศฟังก์ชัน/คลาส/จัดการ I/O

หากต้องการทำโปรเจกต์ต่อ สามารถเริ่มจาก:

1. สร้างไฟล์ `.tt`
2. `import` library ที่ต้องการ
3. ใช้ `link` สำหรับไฟล์ย่อย
4. Compile ด้วย `-target=exe`
5. ทำ C/C++ FFI ถ้าต้องการขยายระบบ

## หมายเหตุ

โครงการนี้ออกแบบให้เป็น compiler front-end + LLVM IR backend แบบตั้งแต่ตัวเอง โดยเฉพาะส่วนจัดการ `link` และ `FFI` ได้ถูกปรับให้ใช้งานได้จริงในโฟลเดอร์โปรเจกต์และไฟล์เอกสารต่าง ๆ
