# ModuleManager สำหรับ ThaiThon

## วัตถุประสงค์

ModuleManager เป็นเครื่องมือสำหรับจัดการ library/module ของ ThaiThon แบบสากล

- ติดตั้ง module จาก GitHub หรือ Git repository อื่น ๆ โดยใช้ `git`
- เก็บ module ในโฟลเดอร์ `lib/` ของ compiler
- เก็บ module ใน project-local `lib/` และ link เข้ากับ project
- ตรวจ version และความเข้ากันได้กับ compiler
- remove / uninstall / update / search / list

## โครงสร้างที่ใช้

```text
ThaiThon/
├─ lib/                     # module ของ compiler
├─ lib_header.json
├─ config.json              # version ของ compiler
├─ src/
│  ├─ ModuleManager.java
│  └─ Json.java
├─ test/
│  └─ part8_module_manager/
│     ├─ module_repo/
│     │  ├─ lib.json
│     │  └─ greet.c
│     └─ project_demo/
│        ├─ main.tt
│        └─ lib.json
└─ README_ModuleManager.md
```

## โหมดใช้งาน

### 1. permanent mode

ติดตั้งไว้ใน compiler root

```bash
java -cp ./bin ModuleManager install greet --repo=https://github.com/owner/thaithon-greet.git --mode=permanent
```

ผลลัพธ์:

```text
<compiler_root>/lib/greet/
```

### 2. project mode

ติดตั้งสำหรับ project ปัจจุบัน และเขียนลง `lib.json`

```bash
java -cp ./bin ModuleManager install greet --repo=https://github.com/owner/thaithon-greet.git --mode=project
```

ผลจะทำให้มี:

```text
<project_root>/lib/greet/
<project_root>/lib.json
```

## lib.json ของ Module

Module repo ต้องมี `lib.json` ที่ระบุข้อมูลดังนี้

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

### สิ่งที่ ModuleManager ตรวจ

- `version` ของ module
- `compilerVersion` ของ module
- เปรียบเทียบกับ `config.json` ของ compiler
- ถ้า module ต้องการ compiler version สูงกว่าเครื่องปัจจุบัน จะ error

## ตัวอย่าง Version Check

```json
{
  "version": "2.1.0",
  "compilerVersion": "0.0.1"
}
```

ถ้า `config.json` มี:

```json
{
  "version": "0.0.1"
}
```

module จะผ่านได้ เพราะ compiler version ตรงหรือสูงกว่า

ถ้า module ระบุ:

```json
{
  "compilerVersion": "1.0.0"
}
```

และ compiler version ปัจจุบันคือ `0.0.1` ModuleManager จะหยุดทำงานทันที พร้อมข้อความ error

## ตัวอย่างการใช้งานเต็ม

### ติดตั้งแบบ project

```bash
java -cp ./bin ModuleManager install greet --repo=https://github.com/owner/thaithon-greet.git --mode=project
```

### ค้นหา module

```bash
java -cp ./bin ModuleManager search greet
```

### อัปเดต module

```bash
java -cp ./bin ModuleManager update greet --version=1.1.0
```

### ลบ module

```bash
java -cp ./bin ModuleManager uninstall greet
```

หรือ

```bash
java -cp ./bin ModuleManager remove greet
```

### เชื่อม module กับ project

```bash
java -cp ./bin ModuleManager link greet
```

### แสดงรายการที่ติดตั้งแล้ว

```bash
java -cp ./bin ModuleManager list
```

## ตัวอย่าง project demo

ไฟล์ตัวอย่างอยู่ที่

```text
test/part8_module_manager/project_demo/main.tt
```

```thai
import "greet";
greet.hi("ThaiThon");
```

และ `lib.json` ของ project

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

## ตัวอย่าง repo module demo

ไฟล์ repo demo อยู่ที่

```text
test/part8_module_manager/module_repo/lib.json
```

ซึ่งเป็น module repo ที่ ModuleManager จะใช้ `git clone` แล้วคัดลอกลง `lib/` ให้กับ compiler หรือ project

## การทดสอบจริง

```bash
cd /d/Project/part3/ThaiThon
java -cp ./bin ModuleManager help
java -cp ./bin ModuleManager search greet
```

สำหรับ repo ทดสอบ local แนะนำใช้:

```bash
git clone <repo-url> ./tmp/module-test
```

หรือถ้าจะใช้ repo ตัวอย่างภายใน workspace:

```bash
java -cp ./bin ModuleManager install greet --repo=D:/Project/part3/ThaiThon/test/part8_module_manager/module_repo --mode=project
```

## สรุป

ModuleManager ช่วยให้ ThaiThon มีระบบ package manager อย่างง่ายแบบ Git-based โดยมีหลักการ:

- โหลด module จาก git
- ตรวจ version + compiler match
- จัดเก็บลง `lib/`
- link กับ project เพียงผ่าน `lib.json`
- ใช้งานได้ทั้ง compiler หลักและ project-local
