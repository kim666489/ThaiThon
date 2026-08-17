# ThaiThon ModuleManager — คู่มือฉบับละเอียด

`ModuleManager` คือ package manager แบบ Git-based ของ ThaiThon เขียนด้วย Java (`src/ModuleManager.java` + `src/Json.java`, รวม ~2,000 บรรทัด) เอกสารนี้อธิบายพฤติกรรมจริงของทุกคำสั่ง วิธี resolve path, การตรวจ version, และกลไก error-handling/rollback ที่ฝังอยู่ในโค้ด โดยอ้างอิงจากซอร์สโดยตรง ไม่ใช่แค่สรุปแบบผิวเผิน

---

## สารบัญ

1. [ภาพรวมและเป้าหมายการออกแบบ](#ภาพรวมและเป้าหมายการออกแบบ)
2. [การ build และรัน](#การ-build-และรัน)
3. [การ detect โฟลเดอร์ root สองแบบ](#การ-detect-โฟลเดอร์-root-สองแบบ)
4. [คำสั่งทั้งหมด](#คำสั่งทั้งหมด)
5. [`install` แบบละเอียด](#install-แบบละเอียด)
6. [`update` แบบละเอียด](#update-แบบละเอียด)
7. [`uninstall` / `remove`](#uninstall--remove)
8. [`search`](#search)
9. [`link`](#link)
10. [`list`](#list)
11. [schema ของ `lib.json` ใน module repo](#schema-ของ-libjson-ใน-module-repo)
12. [การตรวจสอบ compiler version](#การตรวจสอบ-compiler-version)
13. [`module.json`: metadata ที่เขียนไว้หลังติดตั้ง](#modulejson-metadata-ที่เขียนไว้หลังติดตั้ง)
14. [`lib.json` ของ project หลัง link](#libjson-ของ-project-หลัง-link)
15. [กลไกความปลอดภัยของข้อมูล (rollback / timeout / cleanup)](#กลไกความปลอดภัยของข้อมูล-rollback--timeout--cleanup)
16. [GitHub token สำหรับ `search`](#github-token-สำหรับ-search)
17. [ตัวอย่างครบวงจรจาก `test/part8_module_manager/`](#ตัวอย่างครบวงจรจาก-testpart8_module_manager)
18. [ตารางสรุป error message ที่พบบ่อย](#ตารางสรุป-error-message-ที่พบบ่อย)

---

## ภาพรวมและเป้าหมายการออกแบบ

`ModuleManager` ทำหน้าที่:

- ติดตั้ง module จาก Git repository (GitHub หรือที่อื่น) ด้วยการเรียก `git` เป็น subprocess
- เก็บ module ไว้ได้สองที่: **compiler-permanent** (`<compiler_root>/lib/`) หรือ **project-local** (`<project_root>/lib/`)
- ตรวจ compatibility ระหว่าง compiler version ปัจจุบันกับ `compilerVersion` ที่ module ต้องการ ก่อนติดตั้งทุกครั้ง
- รองรับ `install` / `update` / `uninstall` (alias `remove`) / `search` / `link` / `list` / `help`
- ทำงานได้อย่างปลอดภัยแม้ network/`git` ล้มเหลวกลางทาง (มี timeout, rollback, และ temp-dir cleanup ฝังอยู่ในทุก path)

> โค้ดมีคอมเมนต์ `BUG-XX fix` กำกับอยู่หลายจุด ซึ่งบันทึกไว้ว่าพฤติกรรมปัจจุบันถูกออกแบบมาเพื่อแก้ปัญหาอะไรมาก่อน เอกสารนี้จะอ้างอิงเลข BUG เหล่านั้นเพื่อให้เห็นเหตุผลเชิง design ที่ชัดเจน

---

## การ build และรัน

```bash
make build_tmi                 # javac -d ./bin ./src/*.java
java -cp ./bin ModuleManager <command> [args...]
```

หรือผ่าน makefile โดยตรง:

```bash
make run_tmi args="install greet --repo=https://github.com/owner/thaithon-greet.git --mode=project"
```

ตอน `main()` เริ่มทำงาน จะโหลด config จาก `config/client.properties` (ผ่าน `ConfigProp.load(...)`) ก่อนเสมอ ถ้าไฟล์นี้หายไปโปรแกรมจะ error ตั้งแต่ต้น

---

## การ detect โฟลเดอร์ root สองแบบ

`ModuleManager` แยกแยะ root สองแบบที่ไม่เหมือนกัน และมีวิธี detect ต่างกัน:

### 1. `MotherPath` — compiler root

หาโดย `detectProjectRoot()`:

- เริ่มจาก location ของ `.class`/`.jar` ที่กำลังรันอยู่ แล้วเดินขึ้นไปสูงสุด 6 ระดับ
- ถือว่าเจอ compiler root เมื่อโฟลเดอร์นั้นมีทั้ง `src/` และ `config/client.properties` (เช็คใน `looksLikeCompilerRoot()`)
- ถ้าหาไม่เจอเลยจน depth หมด จะ fallback ไปที่ current working directory พร้อม print คำเตือน:
  ```text
  [Warn] Could not reliably detect the ThaiThon compiler root from the running class location; falling back to the current working directory: <path>
  ```

(หมายเหตุจากคอมเมนต์ `BUG-07 fix`: เวอร์ชันก่อนหน้าเดินขึ้นไปตายตัว 3 ระดับ ซึ่งใช้ได้แค่กับ layout `bin/ModuleManager.class` เท่านั้น และ fallback แบบเงียบ ๆ โดยไม่มี warning เลย)

### 2. `ProjectRoot` — project directory ของผู้ใช้

หาโดย `findProjectRootFromCwd()`:

- เริ่มจาก current working directory แล้วเดินขึ้นไปเรื่อย ๆ
- ถือว่าเจอ project root เมื่อโฟลเดอร์นั้นมีทั้ง `src/` และ `lib/`
- ถ้าไม่เจอเลยจะใช้ current working directory ตรง ๆ เป็น fallback

`CompilerLibDir = MotherPath.resolve("lib")` และ `ProjectLibFile = ProjectRoot.resolve("lib.json")` ถูกคำนวณจากค่าทั้งสองนี้ตั้งแต่ `main()` เริ่มทำงาน และใช้เป็นฐานของทุกคำสั่งที่เหลือ

---

## คำสั่งทั้งหมด

```text
java -cp ./bin ModuleManager install <module> [version] --repo=<git-url> [--mode=permanent|project]
java -cp ./bin ModuleManager update <module> [version] [--repo=<git-url>]
java -cp ./bin ModuleManager uninstall <module>
java -cp ./bin ModuleManager remove <module>
java -cp ./bin ModuleManager search <keyword>
java -cp ./bin ModuleManager link <module>
java -cp ./bin ModuleManager list
java -cp ./bin ModuleManager help | --help | -h
```

ถ้าไม่ส่ง argument ใด ๆ เลย หรือใช้ command ที่ไม่รู้จัก จะพิมพ์ help และ (เฉพาะกรณี unknown command) exit ด้วย code `2`

---

## `install` แบบละเอียด

### รูปแบบ flag ที่ `installModule()` รับ

| flag | ความหมาย |
|---|---|
| `<module>` (positional แรก) | ชื่อ module ที่จะติดตั้ง |
| `--repo=<url>` | Git URL ของ module repo (ดูหัวข้อ resolve URL ด้านล่างถ้าไม่ระบุ) |
| `--mode=permanent\|project` | ติดตั้งไว้ที่ `MotherPath/lib` (default) หรือ `ProjectRoot/lib` |
| `--version=<v>` หรือ `-v<v>` หรือ token เปล่า ๆ ที่หน้าตาเหมือน version (เช่น `1.2.3`, `v1.2.3`) | ระบุ version ที่ต้องการ |

### ขั้นตอนภายใน (ตามลำดับจริงในโค้ด)

1. เลือก `targetRoot` ตาม `--mode` แล้ว `Files.createDirectories(targetRoot)`
2. เรียก `fetchModuleMetadata(name, version, repoUrl)` (ดูรายละเอียดด้านล่าง) — ขั้นตอนนี้จะ `git clone` ไป temp directory ก่อนเสมอ
3. ตรวจ `compilerVersion` ที่ module ต้องการ เทียบกับ `config.json` ของ compiler ปัจจุบัน (ผ่าน `validateCompilerVersion`) — ถ้าไม่ผ่านจะ throw error และ**หยุดทันที** ก่อนติดตั้งอะไรทั้งสิ้น
4. เช็คว่ามี module ชื่อนี้ติดตั้งอยู่แล้วใน `targetRoot` หรือไม่:
   - **ถ้ามีอยู่แล้วและ version ตรงกัน (หรือไม่ได้ระบุ version)** → พิมพ์ "Module already installed" และจบ (ถ้า `--mode=project` จะยัง `linkModuleToProject()` ให้เพื่อความชัวร์ว่า `lib.json` มี entry อยู่)
   - **ถ้ามีอยู่แล้วแต่ version ต่างกัน** → เรียก `replaceInstalledModule()` เพื่อสลับเวอร์ชัน (ดูหัวข้อ rollback ด้านล่าง)
   - **ถ้ายังไม่มี** → `copyDirectoryExcludingGit()` คัดลอกจาก temp clone ไปยัง `installDir` แล้วเขียน `module.json` ด้วย `writeModuleIndex()`
5. ถ้า `--mode=project` → เรียก `linkModuleToProject()` เพื่อเขียน/อัปเดต entry ใน `<project_root>/lib.json`
6. `finally` block เรียก `cleanupTempDir()` เสมอ ไม่ว่าจะสำเร็จหรือ error (คอมเมนต์ `BUG-01 fix`: เวอร์ชันก่อนหน้ามี code path ที่ leak temp directory ทิ้งไว้)

### การ resolve `--repo=` ถ้าไม่ระบุ (`guessGitHubUrl`)

- ถ้าใน argument ไม่มี `--repo=` แต่ module name มี `/` อยู่ (เช่น `owner/repo`) → จะเดา URL เป็น `https://github.com/owner/repo.git` ให้อัตโนมัติ
- ถ้า module name **ไม่มี** `/` เลย (เช่นแค่ `greet`) → **จะ error ทันที** ไม่เดา URL แบบสุ่มสี่สุ่มห้า:

  ```text
  Cannot resolve a repository for module "greet": no --repo=<url> was given and "greet" is not an
  "owner/repo" shorthand or a full git URL. Pass --repo=<git-url> explicitly.
  ```

  (คอมเมนต์ `BUG-08 fix`: เวอร์ชันก่อนหน้าเคยเดาว่า repo อยู่ใต้ org `ThaiThon` ที่ไม่มีอยู่จริง ทำให้ error 404 อย่างงง ๆ เวอร์ชันปัจจุบันจึง fail fast พร้อมข้อความชัดเจนแทน)

### การ clone และ checkout version (`fetchModuleMetadata`)

- ถ้า**ไม่ระบุ version** → ใช้ shallow clone (`git clone --depth 1`) เพื่อความเร็ว
- ถ้า**ระบุ version** → ต้องทำ **full clone** (ไม่ใช้ `--depth 1`) เพราะ shallow clone มีแค่ tip ของ default branch เท่านั้น ไม่มี tag/branch อื่นให้ checkout (คอมเมนต์ `BUG-02 fix`)
- หลัง clone แล้วจะเรียก `checkoutRequestedVersion()` ซึ่งลอง checkout ทั้งสองรูปแบบ: ค่าที่ระบุตรง ๆ (`1.2.3`) และแบบมี `v` นำหน้า (`v1.2.3`) — ถ้า checkout ไม่สำเร็จเลยทั้งสองแบบจะ error:
  ```text
  Requested version "<v>" was not found as a git tag or branch in the module repository. Tried: <v>, v<v>
  ```

### การอ่าน `lib.json` ของ module (`parseModuleMetadata`)

หลัง clone/checkout เสร็จ จะหาไฟล์ `lib.json` แรกที่เจอในโฟลเดอร์ repo (`findFirstFile`) แล้ว parse ตามลำดับความสำคัญนี้:

1. ถ้า `lib.json` มี key ตรงกับชื่อ module ที่ระบุตอน install เป๊ะ ๆ → ใช้ entry นั้น
2. ถ้าไม่มี key ตรงชื่อ แต่ root ของ JSON เองมี field `name`/`version`/`compilerVersion`/`compiler` → ถือว่าทั้งไฟล์คือ module เดียวเลย
3. ถ้าไม่เข้าเงื่อนไขไหนเลย → สแกนหา key ที่ "หน้าตาเหมือน module" (มี `source`/`functions`/`declare`/`version` อย่างใดอย่างหนึ่ง):
   - ถ้าเจอ**เพียงตัวเดียว** → ใช้ตัวนั้น พร้อม warning ว่าชื่อไม่ตรงกับที่ขอ
   - ถ้าเจอ**มากกว่าหนึ่ง** → **error ทันที** ไม่เดาว่าจะใช้ตัวไหน (คอมเมนต์ `BUG-11 fix`: ป้องกันการติดตั้ง module ผิดตัวแบบเงียบ ๆ)

### version ที่บันทึกจริงหลัง install

- ถ้า `lib.json` ที่ checkout มามี field `"version"` → ใช้ค่านั้นเสมอ (แม้จะต่างจาก version ที่ผู้ใช้ขอ ก็จะ warning แต่ยังใช้ค่าจาก `lib.json`)
- ถ้า `lib.json` ไม่มี `"version"` แต่ผู้ใช้ระบุ version มา → บันทึกค่าที่ผู้ใช้ระบุ พร้อม warning ว่ายังไม่ได้ verify
- ถ้าไม่มีทั้งคู่ → บันทึกเป็น `"0.0.0"`

---

## `update` แบบละเอียด

```bash
java -cp ./bin ModuleManager update greet --version=1.1.0
```

ขั้นตอน:

1. หา module ที่ติดตั้งอยู่แล้วด้วย `findInstalledModuleDir()` (ค้นทั้ง project-local และ compiler-permanent — ดูหัวข้อถัดไป)
2. ถ้า**ไม่เจอเลย** → พิมพ์ "Module not found locally... Running install instead." แล้วเรียก `installModule()` แทน
3. ถ้าเจอ → `fetchModuleMetadata()` ใหม่ (clone ซ้ำจาก repo), ตรวจ compiler version, แล้วเรียก `replaceInstalledModule()` เพื่ออัปเดตทับที่ตำแหน่งเดิมที่เจอ

ข้อสำคัญ: `update` **ไม่รับ** `--mode=` flag เลย (คอมเมนต์ `BUG-12 fix`) — เพราะจะอัปเดต module ใน root เดิมที่เจอมันเสมอ ไม่ redirect ไปอีก root หนึ่งโดยไม่ตั้งใจ

---

## `uninstall` / `remove`

```bash
java -cp ./bin ModuleManager uninstall greet
java -cp ./bin ModuleManager remove greet     # alias เดียวกัน
```

ขั้นตอน:

1. หา module ด้วย `findInstalledModuleDir()`
2. ถ้าไม่เจอ → พิมพ์ "Module not installed: <name>" แล้วจบเฉย ๆ (ไม่ error)
3. ถ้าเจอ → `deleteRecursively(moduleDir)` แล้วเรียก `removeProjectLink()` เพื่อลบ entry ของ module นี้ออกจาก `<project_root>/lib.json` ด้วย (ถ้ามี)

---

## `search`

```bash
java -cp ./bin ModuleManager search greet
```

ทำสองอย่างต่อกัน:

1. **Local search** — สแกนชื่อโฟลเดอร์ทั้งใน `CompilerLibDir` และ `ProjectRoot/lib` ที่ชื่อมี keyword เป็น substring (case-insensitive)
2. **Remote search** — ยิง GitHub Search API: `https://api.github.com/search/repositories?q=ThaiThon+<keyword>&per_page=5` (ดูหัวข้อ GitHub token ด้านล่างสำหรับเรื่อง rate limit)

ผลลัพธ์ remote แต่ละรายการแสดงเป็น `<owner/repo> -> <html_url>`

---

## `link`

```bash
java -cp ./bin ModuleManager link greet
```

ใช้เมื่อ module ติดตั้งอยู่แล้ว (permanent หรือ project) แต่ `lib.json` ของ project ปัจจุบันยังไม่มี entry ชี้ไปหามัน:

1. หา module ด้วย `findInstalledModuleDir()` — ถ้าไม่เจอ error `Module not installed: <name>. Run install first.`
2. ตรวจว่ามี `module.json` อยู่ในโฟลเดอร์ที่ติดตั้งจริง (ไม่งั้น error)
3. อ่าน metadata จาก `module.json`, ตรวจว่า module อยู่ใน `project` หรือ `permanent` root จริง ๆ ด้วย `detectInstallMode()` (เทียบ real path ไม่เชื่อ flag ที่ส่งมา — คอมเมนต์ `BUG-13 fix`)
4. เขียน entry ลง `<project_root>/lib.json` ผ่าน `linkModuleToProject()`

---

## `list`

```bash
java -cp ./bin ModuleManager list
```

แสดงรายการ module ที่ติดตั้งแล้วแยกเป็นสองกลุ่ม (ถ้ามี):

```text
Installed modules (permanent @ <compiler_root>/lib):
  - greet @ 1.0.0

Installed modules (project @ <project_root>/lib):
  - othermod @ 2.0.0
```

version ของแต่ละ module อ่านจาก `module.json` ภายในโฟลเดอร์นั้น ถ้าไม่มีไฟล์นี้จะแสดงเป็น `unknown`

---

## schema ของ `lib.json` ใน module repo

Module repo ที่จะใช้ `install` ได้ต้องมี `lib.json` ที่ root (หรือที่ไหนก็ได้ในโฟลเดอร์ที่ clone มา — `findFirstFile` จะหาให้) และมีรูปแบบ:

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

field ที่ `ModuleManager` สนใจโดยตรง:

| field | ใช้ทำอะไร |
|---|---|
| `name` | ชื่อ module ที่จะถูกบันทึกลง `module.json` (ถ้าไม่มีจะใช้ key ของ JSON แทน) |
| `version` | version ที่จะบันทึกไว้ (ถ้าไม่มีจะ fallback ไปใช้ค่าที่ผู้ใช้ระบุตอน install หรือ `"0.0.0"`) |
| `compilerVersion` (หรือ `compiler.version`/`compiler.compilerVersion`) | ใช้เทียบกับ compiler version ปัจจุบันก่อนติดตั้ง |
| `source`, `declare`, `functions` | field เดียวกับที่ใช้ในระบบ FFI ปกติ (ดู [README_FFI.md](./README_FFI.md)) — ไม่ได้ถูก `ModuleManager` ประมวลผลเอง แต่จะถูกใช้ตอนที่ compiler อ่าน `lib.json` ที่ install ไว้ผ่านระบบ import/FFI ตามปกติ |

---

## การตรวจสอบ compiler version

`validateCompilerVersion(required, actual)` ทำงานดังนี้:

1. ถ้า `required` เป็นค่าว่าง → ผ่านทันที (ไม่บังคับ)
2. normalize ทั้งสองค่าด้วย `normalizeVersion()` — ตัด prefix `v`/`V`, ตัด comparator (`>=`, `<=`, `==`, `!=`, `>`, `<`) ออก, แล้วเก็บเฉพาะ run ของตัวเลข-จุดที่ต่อเนื่องกันจากซ้ายสุด (ตัด pre-release/build suffix เช่น `-beta1` ทิ้งทั้งก้อน ไม่ filter ทีละตัวอักษร — คอมเมนต์ `BUG-06 fix`)
3. เทียบด้วย `compareVersions()` แบบ semantic (`major.minor.patch...`, เติม `0` ให้ตำแหน่งที่ขาด)
4. ถ้า `actual < required` → throw:
   ```text
   Compiler version mismatch. Required: <required>, current: <actual>
   ```

`actual` (compiler version ปัจจุบัน) มาจาก `readCompilerVersionFromProject()` ซึ่งอ่าน field `"version"` จาก `<compiler_root>/config.json` โดยตรง (ถ้าไฟล์ไม่มีหรืออ่านไม่ได้จะถือว่าเป็น `"0.0.0"`)

### ตัวอย่าง

`config.json` ของ compiler:

```json
{ "version": "0.0.1" }
```

- module ระบุ `"compilerVersion": "2.1.0"` แต่ `required` note เป็นตัวอย่างเชิงแนวคิด → ถ้า compiler version ปัจจุบันสูงกว่าหรือเท่ากับที่ module ต้องการ ผ่านทันที
- module ระบุ `"compilerVersion": "1.0.0"` และ compiler ปัจจุบันคือ `0.0.1` → **ติดตั้งไม่ผ่าน** พร้อม error message ข้างต้น

---

## `module.json`: metadata ที่เขียนไว้หลังติดตั้ง

หลัง `install`/`update` สำเร็จ `writeModuleIndex()` จะเขียนไฟล์ `module.json` ไว้ในโฟลเดอร์ที่ติดตั้ง:

```json
{
  "name": "greet",
  "version": "1.0.0",
  "compilerVersion": "0.0.1",
  "repoUrl": "https://github.com/owner/thaithon-greet.git",
  "installedAt": "<absolute path ของโฟลเดอร์ที่ติดตั้ง>"
}
```

ไฟล์นี้คือแหล่งข้อมูลที่ `list`, `link`, และ `update` ใช้อ่าน metadata กลับมาโดยไม่ต้อง clone ซ้ำ

---

## `lib.json` ของ project หลัง link

`linkModuleToProject()` จะอ่าน `<project_root>/lib.json` เดิม (ถ้ามี), เพิ่ม/แก้ entry ของ module ที่ระบุ แล้วเขียนกลับ:

```json
{
  "greet": {
    "path": "<absolute path ของโฟลเดอร์ที่ติดตั้งจริง>",
    "version": "1.0.0",
    "compilerVersion": "0.0.1",
    "mode": "project"
  }
}
```

field `"mode"` จะเป็น `"project"` หรือ `"permanent"` ตาม mode จริงที่ module ถูกติดตั้งไว้ (ไม่ hardcode — คอมเมนต์ `BUG-13 fix`: เวอร์ชันก่อนหน้าเคยเขียน `"mode": "permanent"` ลงไปเสมอไม่ว่าจริง ๆ จะติดตั้งแบบไหน ทำให้ข้อมูลใน `lib.json` ไม่ตรงกับความเป็นจริง)

---

## กลไกความปลอดภัยของข้อมูล (rollback / timeout / cleanup)

โค้ดมีกลไกป้องกันความเสียหายฝังอยู่หลายจุด ซึ่งสำคัญมากสำหรับความน่าเชื่อถือของ package manager:

### Swap-then-delete แทน delete-then-copy (`replaceInstalledModule`, `BUG-04 fix`)

เมื่อ `update`/`install` เจอ module เดิมที่ต้องแทนที่ด้วย version ใหม่:

1. ย้ายโฟลเดอร์เดิมไปเป็น `<name>.bak_<timestamp>` ก่อน (ยังไม่ลบ)
2. copy โฟลเดอร์ใหม่เข้าไปแทน
3. เขียน `module.json` ใหม่
4. **ถ้าทุกอย่างสำเร็จ** → ค่อยลบ backup ทิ้ง
5. **ถ้าขั้นตอนไหนล้มเหลว** → restore backup กลับที่เดิมทันที (rollback อัตโนมัติ)

ผลคือ module ที่ติดตั้งอยู่จะไม่มีทาง "หายไปครึ่งทาง" แม้ระหว่าง copy จะเกิด error

### Temp directory cleanup ที่รับประกัน (`cleanupTempDir`, `BUG-01 fix`)

`fetchModuleMetadata()` clone ไปที่ temp directory เสมอ และ `installModule()`/`updateModule()` เรียก `cleanupTempDir()` ใน `finally` block เสมอ — รับประกันว่า temp dir จะถูกลบไม่ว่าคำสั่งจะสำเร็จ, ล้มเหลวตอน validate version, หรือ short-circuit เพราะ "already installed" ก็ตาม

### Git subprocess timeout (`runGitCommand`, `BUG-09 fix`)

ทุกคำสั่ง `git` ที่เรียกผ่าน `runGitCommand()`:

- ตั้ง `GIT_TERMINAL_PROMPT=0` เพื่อไม่ให้ค้างรอ auth prompt (เช่น repo private ที่ไม่มีสิทธิ์)
- มี hard timeout ที่ **60 วินาที** (`GIT_TIMEOUT_SECONDS`) — ถ้าเกินจะ `destroyForcibly()` process แล้ว throw `IOException` ทันที

### `.git/` ไม่ถูกคัดลอกเข้าไปในโมดูลที่ติดตั้ง (`copyDirectoryExcludingGit`, `BUG-05 fix`)

ตอนคัดลอกจาก temp clone ไปยังโฟลเดอร์ที่ติดตั้งจริง จะข้ามทุก path ที่ขึ้นต้นด้วย `.git` เพื่อไม่ให้ object database ทั้งก้อนของ git ถูกก็อปตามไปด้วย (ลดขนาด และป้องกันปัญหา nested git repo)

### หา module ทั้งสอง root ก่อนสรุปว่าไม่เจอ (`findInstalledModuleDir`, `BUG-03 fix`)

`update`/`uninstall`/`link` ทั้งหมดค้นหาทั้ง `ProjectRoot/lib` และ `CompilerLibDir` เสมอ ถ้าพบชื่อเดียวกันทั้งสองที่จะ**ใช้ project-local ก่อน** พร้อม warning ว่ามีทั้งสองที่ (ไม่ใช่ error — ผู้ใช้ต้องตัดสินใจเองว่าจะจัดการอย่างไรต่อ)

---

## GitHub token สำหรับ `search`

GitHub Search API แบบไม่ authenticate จำกัดที่ 60 request/ชั่วโมงต่อ IP ซึ่งหมดง่ายมาก (`BUG-14 fix`) `findLocalGitHubToken()` จึงพยายามหา credential ที่มีอยู่แล้วในเครื่องตามลำดับนี้:

1. environment variable `GITHUB_TOKEN` หรือ `GH_TOKEN`
2. คำสั่ง `gh auth token` (ถ้าติดตั้ง GitHub CLI และ login ไว้แล้ว)
3. `git credential fill` สำหรับ host `github.com` (credential helper เดียวกับที่ `git clone`/`git push` ใช้ เช่น OS keychain, `wincred`)

ถ้าเจอ token จะยิง request แบบ authenticated (limit เพิ่มเป็น 5,000 request/ชั่วโมง) ถ้าไม่เจอเลยจะ fallback ไปแบบ anonymous พร้อม warning (เฉพาะตอน `debug=true`) ว่าทำไมถึงถูกจำกัด

ถ้าเจอ HTTP 403/429 (rate limited) จะพิมพ์คำแนะนำ:

```text
[Warn] GitHub search rate-limited (HTTP 403). Log in with `gh auth login` or set GITHUB_TOKEN to raise the limit.
```

---

## ตัวอย่างครบวงจรจาก `test/part8_module_manager/`

```text
test/part8_module_manager/
├─ module_repo/            # จำลอง Git module repo (มี lib.json + greet.c)
│  └─ lib.json
└─ project_demo/            # จำลองโปรเจกต์ที่ใช้ module นี้
   ├─ main.tt
   └─ lib.json
```

`module_repo/lib.json`:

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

`project_demo/main.tt`:

```thai
import "greet";

greet.hi("ThaiThon");
```

`project_demo/lib.json` (สถานะหลัง `install --mode=project` + `link` สำเร็จ):

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

### ทดสอบด้วย local repo (ไม่ต้องพึ่ง GitHub จริง)

เนื่องจาก `--repo=` รับ path ธรรมดาได้ (ไม่จำเป็นต้องเป็น URL ระยะไกล เพราะ `git clone` เองก็รองรับ local path):

```bash
java -cp ./bin ModuleManager install greet \
  --repo=/path/to/ThaiThon/test/part8_module_manager/module_repo \
  --mode=project
```

จากนั้นตรวจสอบด้วย:

```bash
java -cp ./bin ModuleManager list
java -cp ./bin ModuleManager search greet
```

---

## ตารางสรุป error message ที่พบบ่อย

| ข้อความ | สาเหตุ | แก้ไข |
|---|---|---|
| `install requires module name` | ไม่ได้ระบุชื่อ module ต่อท้ายคำสั่ง `install` | ระบุชื่อ module เช่น `install greet ...` |
| `Cannot resolve a repository for module "<name>": ...` | ไม่ได้ใส่ `--repo=` และชื่อ module ไม่ใช่รูปแบบ `owner/repo` | เพิ่ม `--repo=<git-url>` |
| `Requested version "<v>" was not found as a git tag or branch...` | tag/branch ที่ระบุไม่มีอยู่จริงใน repo | ตรวจ tag ที่มีจริงด้วย `git ls-remote --tags <repo>` |
| `Compiler version mismatch. Required: <r>, current: <a>` | module ต้องการ compiler version สูงกว่าที่มีอยู่ | อัปเกรด compiler หรือใช้ module เวอร์ชันเก่ากว่า |
| `Repository does not contain lib.json: <url>` | repo ที่ clone มาไม่มีไฟล์ `lib.json` เลย | เพิ่ม `lib.json` ให้ module repo ก่อน publish |
| `lib.json defines multiple modules (...) and none match the requested name "<name>"` | `lib.json` มีหลาย module key แต่ไม่มีตัวไหนตรงกับชื่อที่ระบุตอน install | ระบุชื่อ module ให้ตรงกับ key จริงใน `lib.json` |
| `Module not installed: <name>. Run install first.` | เรียก `link` กับ module ที่ยังไม่เคย `install` | รัน `install` ก่อน |
| `module.json missing in installed module: <name>` | โฟลเดอร์ module มีอยู่แต่ไม่มี `module.json` (อาจถูกแก้ไขเองด้วยมือ) | ลบแล้ว `install` ใหม่ |
| `Git command timed out after 60s: git <args>` | `git` subprocess ค้างเกิน timeout (เช่น รอ auth prompt ที่ถูกปิดไว้) | ตรวจสิทธิ์เข้าถึง repo, ตรวจว่าไม่ใช่ private repo ที่ไม่มี credential |
| `Git command failed (exit <n>): <output>` | คำสั่ง `git` ล้มเหลว (URL ผิด, ไม่มีสิทธิ์, network ล่ม ฯลฯ) | อ่าน `<output>` เพื่อดูสาเหตุจาก git โดยตรง |