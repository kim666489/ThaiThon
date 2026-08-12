#include <iostream>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <thread>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <random>
#include <cstdint>
#include <cstdlib>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#include "nlohmann/json.hpp"
#include "Mono_c11.hpp"
#include "Parser.hpp"

using namespace std;
namespace fs = std::filesystem;
using json = nlohmann::json;

// NOTE: this file calls MONO_HPP::load_rule(...) and MONO_HPP::Lexer(...).
// The mono.hpp shared earlier does NOT wrap its declarations in
// `namespace MONO_HPP { ... }` — everything in it (load_rule, Lexer,
// Token, Normalizer, ...) is at global scope. As written, this file will
// not compile against that header. Pick ONE of:
//   (a) wrap mono.hpp's body in `namespace MONO_HPP { ... }`, then also
//       qualify Token as MONO_HPP::Token everywhere it's used (Parser.hpp
//       included — it currently uses bare `Token`), or
//   (b) drop the `MONO_HPP::` prefix here and call load_rule(...) /
//       Lexer(...) directly.
// I've left the MONO_HPP:: calls below as-is since that's what your file
// already had; say the word and I'll do (a) or (b) for you.

// ---------- โครงสร้างเก็บค่า config แต่ละตัว ----------
struct ConfigMapNode {
    string _type;          // "bool" | "int" | "string"
    bool BoolValue = false;
    long long IntValue = 0;
    string StringValue;
};

// ---------- ตัวแปร global ----------
map<string, ConfigMapNode> ConfigMap = {
    { "debug",    { "bool", true } }, // เปิด debug เป็น true ไว้เริ่มต้น
    { "-m",       { "string"     } },
    { "-i",       { "string"     } },
    { "-o",       { "string"     } },
    { "version",  { "string"     } },
    // ---- ตัวเลือกสำหรับระบบ compile ผ่าน LLVM ----
    { "-target",  { "string"     } }, // "ir" | "asm" | "exe"  (ค่าเริ่มต้น = "ir")
    { "-llc",     { "string"     } }, // path/ชื่อ binary ของ llc  (ค่าเริ่มต้น = "llc")
    { "-clang",   { "string"     } }, // path/ชื่อ binary ของ clang (ค่าเริ่มต้น = "clang")
    { "-keep-ir", { "bool"       } }  // เก็บไฟล์ .ll ตัวกลางไว้ไหมเมื่อ target != ir (ค่าเริ่มต้น = false)
};

fs::path exePath;                 // path ของโฟลเดอร์ exe
fs::path WalkRulePath;
json PathConfig = { {"configPath", "config.json"} };
json config;                      // เก็บค่า JSON ที่อ่านจากไฟล์

// ---------- helper: split string ----------
vector<string> split(const string& s, char delim) {
    vector<string> tokens;
    string token;
    for (char c : s) {
        if (c == delim) {
            tokens.push_back(token);
            token.clear();
        } else {
            token += c;
        }
    }
    tokens.push_back(token);
    return tokens;
}

// ---------- อ่านค่าเริ่มต้นจากไฟล์ config ----------
void ReadConfigFile() {
    fs::path configFile = exePath / PathConfig["configPath"].get<string>();

    if (!fs::exists(configFile)) {
        cerr << "[Warning] Config file not found at " << configFile << ", using defaults." << endl;
        return;
    }

    ifstream _Config(configFile);
    if (!_Config.is_open()) {
        cerr << "[Warning] Cannot open config file: " << configFile << endl;
        return;
    }

    try {
        _Config >> config;
    } catch (const json::parse_error& e) {
        cerr << "[Error] Parsing config file: " << e.what() << endl;
        return;
    }

    for (auto& [key, node] : ConfigMap) {
        if (!config.contains(key)) continue;

        if (node._type == "bool" && config[key].is_boolean()) {
            node.BoolValue = config[key].get<bool>();
        } else if (node._type == "int" && config[key].is_number_integer()) {
            node.IntValue = config[key].get<long long>();
        } else if (node._type == "string" && config[key].is_string()) {
            node.StringValue = config[key].get<string>();
        }
    }
}

// ---------- override ค่าจาก command line args (key=value) ----------
void configMapping(char* args[], int& argc) {
    int new_argc = 1;

    for (int i = 1; i < argc; i++) {
        string parse(args[i]);
        vector<string> _parse = split(parse, '=');

        if (_parse.size() < 2) {
            args[new_argc++] = args[i];
            continue;
        }

        string key = _parse[0];
        string value = _parse[1];
        for (size_t j = 2; j < _parse.size(); j++) {
            value += "=" + _parse[j];
        }

        auto it = ConfigMap.find(key);
        if (it != ConfigMap.end()) {
            ConfigMapNode& node = it->second;
            if (node._type == "bool") {
                node.BoolValue = (value == "true" || value == "1");
            } else if (node._type == "int") {
                try {
                    node.IntValue = stoll(value);
                } catch (const invalid_argument&) {
                    cerr << "[Warning] Invalid integer value for " << key << endl;
                }
            } else if (node._type == "string") {
                node.StringValue = value;
            }
        } else {
            args[new_argc++] = args[i];
        }
    }
    argc = new_argc;
    args[argc] = nullptr;
}

void ShowLogo() {
    cout << "MatsuVM Version: " << ConfigMap["version"].StringValue << endl;
}

// ---------- แสดงค่า config ปัจจุบัน ----------
void PrintConfig() {
    cout << string(8,'=') << " Config " << string(8,'=') << endl;
    for (auto& [key, node] : ConfigMap) {
        cout << key << " (" << node._type << ") = ";
        if (node._type == "bool") cout << boolalpha << node.BoolValue;
        else if (node._type == "int") cout << node.IntValue;
        else cout << "\"" << node.StringValue << "\"";
        cout << endl;
    }
    cout << string(24,'=') << endl;
}

// =====================================================================
//  ระบบเรียกใช้ LLVM toolchain (llc / clang) ผ่าน command line
//
//  แนวคิด: Parser::run(...) ของโปรเจกต์นี้ generate ไฟล์ .ll (LLVM IR
//  แบบข้อความ) อยู่แล้ว ดังนั้นแทนที่จะ link เข้ากับ libLLVM โดยตรง
//  เราเรียก `llc` (แปลง .ll -> .s/.o) และ `clang` (เป็น linker driver)
//  เป็น external process แทน
//
//  ใหม่: นอกจาก .o ของโปรแกรม ThaiThon เองแล้ว ตอนนี้ยัง compile+link ไฟล์
//  C/C++ ภายนอกที่ถูกประกาศผ่าน lib_header.json's "source" (ดู Parser.hpp)
//  เข้าไปด้วยตอน -target=exe ทำให้ ThaiThon เรียกฟังก์ชันที่เขียนด้วย
//  C/C++ จริง ๆ ได้ ไม่ใช่แค่ libc อย่าง puts()
// =====================================================================

// ---------- ใส่ quote ให้ path กัน space/อักขระพิเศษตอนยิงเข้า shell ----------
static string QuotePath(const fs::path& p) {
    string s = p.string();
    string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += "\"";
    return out;
}

// ---------- เช็คว่ามี binary นี้ใช้ได้ใน PATH ไหม ----------
static bool ToolAvailable(const string& tool) {
#ifdef _WIN32
    string cmd = "where " + tool + " > NUL 2>&1";
#else
    string cmd = "command -v " + tool + " > /dev/null 2>&1";
#endif
    return system(cmd.c_str()) == 0;
}

// ---------- ยิง shell command แล้วเช็ค exit code ----------
static bool RunShellCommand(const string& cmd) {
    if (ConfigMap["debug"].BoolValue) {
        cout << "[Debug] $ " << cmd << endl;
    }
    int rc = system(cmd.c_str());
    if (rc == -1) return false;
#ifndef _WIN32
    if (WIFEXITED(rc)) return WEXITSTATUS(rc) == 0;
    return false; // โดน signal ฆ่า ฯลฯ
#else
    return rc == 0;
#endif
}

// ---------- IR -> Assembly ด้วย llc ----------
static bool CompileIRToAsm(const fs::path& irPath, const fs::path& asmPath, const string& llcTool) {
    if (!ToolAvailable(llcTool)) {
        cerr << "[Error] LLVM tool '" << llcTool
             << "' not found in PATH. Install LLVM (llc) or pass -llc=<path>." << endl;
        return false;
    }
    string cmd = "\"" + llcTool + "\" -filetype=asm " + QuotePath(irPath) + " -o " + QuotePath(asmPath);
    return RunShellCommand(cmd);
}

// ---------- IR -> Object file ด้วย llc (ใช้เป็นขั้นกลางก่อน link) ----------
static bool CompileIRToObject(const fs::path& irPath, const fs::path& objPath, const string& llcTool) {
    if (!ToolAvailable(llcTool)) {
        cerr << "[Error] LLVM tool '" << llcTool
             << "' not found in PATH. Install LLVM (llc) or pass -llc=<path>." << endl;
        return false;
    }
    // -relocation-model=pic: ให้ตรงกับที่ clang จะ link เป็น PIE บน Linux สมัยใหม่
    string cmd = "\"" + llcTool + "\" -relocation-model=pic -filetype=obj "
                 + QuotePath(irPath) + " -o " + QuotePath(objPath);
    return RunShellCommand(cmd);
}

// ---------- NEW: compile ไฟล์ C/C++ ภายนอก (จาก lib_header.json "source") เป็น .o ----------
// ใช้ clang ทั้ง .c และ .cpp ได้เลย (clang จะเลือก frontend ให้เองตามนามสกุลไฟล์)
static bool CompileExternSourceToObject(const fs::path& srcPath, const fs::path& objPath, const string& clangTool) {
    if (!fs::exists(srcPath)) {
        cerr << "[Error] Linked C/C++ source file not found: " << srcPath
             << " (check the \"source\" path in lib_header.json)" << endl;
        return false;
    }
    if (!ToolAvailable(clangTool)) {
        cerr << "[Error] Clang not found in PATH. Install LLVM/Clang or pass -clang=<path>." << endl;
        return false;
    }
    string cmd = "\"" + clangTool + "\" -c " + QuotePath(srcPath) + " -o " + QuotePath(objPath);
    return RunShellCommand(cmd);
}

// ---------- Object file(s) -> Executable ด้วย clang (เป็น linker driver) ----------
// รับ object file ได้หลายไฟล์ในคราวเดียว เพื่อรองรับ .o ของ extern C/C++ sources
// และรับรายชื่อ system library (จาก lib_header.json "libs", เช่น "m" สำหรับ
// libm/sqrt) เพื่อใส่ "-l<name>" ให้ linker หา symbol อย่าง sqrt/pow เจอ
static bool LinkObjectsToExecutable(const vector<fs::path>& objPaths, const vector<string>& libs,
                                     const fs::path& exeOutPath, const string& clangTool) {
    if (!ToolAvailable(clangTool)) {
        cerr << "[Error] Clang not found in PATH. Install LLVM/Clang or pass -clang=<path>." << endl;
        return false;
    }
    string cmd = "\"" + clangTool + "\"";
    for (auto& o : objPaths) cmd += " " + QuotePath(o);
    cmd += " -o " + QuotePath(exeOutPath);
    for (auto& l : libs) cmd += " -l" + l;
    return RunShellCommand(cmd);
}

// ---------- IR (+ extern C/C++ sources) -> Executable ----------
// llc แปลง .ll ของ ThaiThon เป็น .o, clang แปลงแต่ละ extern source เป็น .o,
// แล้ว link ทุก .o เข้าด้วยกันเป็น executable เดียว ลบไฟล์ .o ตัวกลางทิ้งเสมอ
static bool CompileIRToExecutable(const fs::path& irPath, const fs::path& exeOutPath,
                                   const vector<fs::path>& externSources, const vector<string>& externLibs,
                                   const string& llcTool, const string& clangTool) {
    fs::path mainObjPath = fs::path(irPath).replace_extension(".o");

    if (!CompileIRToObject(irPath, mainObjPath, llcTool)) {
        return false;
    }

    vector<fs::path> allObjs = { mainObjPath };
    vector<fs::path> externObjs; // เก็บไว้ลบทีหลัง
    bool ok = true;

    for (auto& src : externSources) {
        // เขียน .o ของ extern source ไว้ข้าง ๆ ไฟล์ .ll ตัวกลาง (ไม่ใช่ข้าง ๆ
        // ไฟล์ source ต้นฉบับ) กันกรณี source directory เป็น read-only และ
        // กันชื่อไฟล์ชนกันระหว่าง source หลายไฟล์ที่ชื่อซ้ำกันจากคนละโฟลเดอร์
        fs::path objName = fs::path(src).filename().replace_extension(".o");
        fs::path objPath = irPath.parent_path() / objName;

        cout << "[Info] Compiling linked source: " << src << endl;
        if (!CompileExternSourceToObject(src, objPath, clangTool)) {
            ok = false;
            break;
        }
        allObjs.push_back(objPath);
        externObjs.push_back(objPath);
    }

    if (ok) {
        ok = LinkObjectsToExecutable(allObjs, externLibs, exeOutPath, clangTool);
    }

    std::error_code ec;
    fs::remove(mainObjPath, ec);
    for (auto& o : externObjs) fs::remove(o, ec);

    return ok;
}

// ---------- คอมไพล์ไฟล์ input เป็น LLVM IR / Assembly / Executable ----------
//
// เมื่อเทียบกับเวอร์ชันก่อนหน้า:
//   - เพิ่มการดึง externSources จาก parser.getExternSources() หลัง parse
//     เสร็จ (มาจาก "source" ใน lib_header.json ของทุก library ที่ import)
//   - target=exe: externSources ทุกไฟล์จะถูก compile+link เข้าไปด้วย
//   - target=ir/asm: ถ้ามี externSources จะแค่เตือนผู้ใช้ว่ายังไม่ link
//     ให้อัตโนมัติในโหมดนี้ (เพราะ .ll เดี่ยว ๆ หรือ .s เดี่ยว ๆ ไม่ใช่
//     รูปแบบที่ link รวมกับ .o ของภาษาอื่นได้ตรง ๆ) ผู้ใช้ต้อง compile/link
//     ไฟล์เหล่านั้นเองถ้าต้องการ target ที่ไม่ใช่ exe
void RunCompiler() {
    const string& mode = ConfigMap["-m"].StringValue;
    if (mode != "c") {
        cout << "[Info] No mode selected or mode '" << mode << "' not recognized. Use -m=c" << endl;
        return;
    }

    const string& inputPath = ConfigMap["-i"].StringValue;
    if (inputPath.empty()) {
        cerr << "[Error] Input file (-i) is required but not specified." << endl;
        return;
    }

    ifstream file(inputPath);
    if (!file.is_open()) {
        cerr << "[Error] Cannot open input file: " << inputPath << endl;
        return;
    }
    stringstream code;
    code << file.rdbuf();
    file.close();

    // -- Lex ------------------------------------------------------------
    vector<Token> tokens;
    try {
        MONO_HPP::load_rule(WalkRulePath.string());
        MONO_HPP::Lexer lexer(code.str(), 4);
        tokens = lexer.run();
    } catch (const std::exception& e) {
        cerr << "[Error] Lexing failed: " << e.what() << endl;
        return;
    }

    if (config.value("showToken", false)) {
        for (Token& t : tokens) {
            cout << t.value << " | " << t.type << " " << t.line << ":" << t.col << endl;
        }
    }

    // -- เลือก target mode ------------------------------------------------
    string target = ConfigMap["-target"].StringValue;
    if (target.empty()) target = "ir";
    if (target != "ir" && target != "asm" && target != "exe") {
        cerr << "[Warning] Unknown -target='" << target
             << "', falling back to 'ir'. Valid values: ir | asm | exe" << endl;
        target = "ir";
    }

    // -- คำนวณ path ของ output สุดท้าย และ path ของ .ll ตัวกลาง -----------
    fs::path finalOutput;
    if (!ConfigMap["-o"].StringValue.empty()) {
        finalOutput = fs::path(ConfigMap["-o"].StringValue);
    } else if (target == "ir") {
        finalOutput = fs::path(inputPath).replace_extension(".ll");
    } else if (target == "asm") {
        finalOutput = fs::path(inputPath).replace_extension(".s");
    } else { // exe
#ifdef _WIN32
        finalOutput = fs::path(inputPath).replace_extension(".exe");
#else
        finalOutput = fs::path(inputPath).replace_extension("");
#endif
    }

    fs::path irPath = (target == "ir") ? finalOutput
                                        : fs::path(finalOutput).replace_extension(".ll");

    // -- Parse + emit LLVM IR --------------------------------------------
    vector<fs::path> externSources;
    vector<string> externLibs;
    try {
        // NEW: folder holding the .tt file being compiled, so Parser can
        // also look for a project-local "lib.json" there (in addition to
        // the current working directory it already checks).
        fs::path projectDir = fs::absolute(fs::path(inputPath)).parent_path();
        Parser parser(tokens, exePath, projectDir);
        parser.run(irPath);
        externSources = parser.getExternSources(); // NEW
        externLibs = parser.getExternLibs();       // NEW
        cout << "[Info] Wrote LLVM IR to " << irPath << endl;
    } catch (const std::exception& e) {
        cerr << "[Error] Compilation failed: " << e.what() << endl;
        return;
    }

    if (target == "ir") {
        if (!externSources.empty()) {
            cout << "[Info] " << externSources.size()
                 << " linked C/C++ source file(s) were referenced but -target=ir "
                    "only emits LLVM IR text; compile/link them yourself, or rerun with -target=exe "
                    "to have them compiled and linked automatically." << endl;
        }
        return; // เสร็จแล้ว ไม่ต้องเรียก LLVM toolchain เพิ่ม
    }

    const string llcTool   = ConfigMap["-llc"].StringValue.empty()   ? "llc"   : ConfigMap["-llc"].StringValue;
    const string clangTool = ConfigMap["-clang"].StringValue.empty() ? "clang" : ConfigMap["-clang"].StringValue;

    bool ok = false;
    if (target == "asm") {
        if (!externSources.empty()) {
            cout << "[Warning] " << externSources.size()
                 << " linked C/C++ source file(s) were referenced but -target=asm only "
                    "produces assembly for the ThaiThon code itself; they are NOT compiled here. "
                    "Use -target=exe to get a fully linked binary." << endl;
        }
        cout << "[Info] Invoking '" << llcTool << "' to generate assembly..." << endl;
        ok = CompileIRToAsm(irPath, finalOutput, llcTool);
        if (ok) cout << "[Info] Wrote assembly to " << finalOutput << endl;
        else    cerr << "[Error] Failed to generate assembly via llc." << endl;
    } else { // exe
        cout << "[Info] Invoking '" << llcTool << "' + '" << clangTool << "' to generate executable...";
        if (!externSources.empty()) cout << " (" << externSources.size() << " linked C/C++ file(s))";
        cout << endl;
        ok = CompileIRToExecutable(irPath, finalOutput, externSources, externLibs, llcTool, clangTool);
        if (ok) {
            cout << "[Info] Wrote executable to " << finalOutput << endl;
#ifndef _WIN32
            std::error_code ec;
            fs::permissions(finalOutput,
                             fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                             fs::perm_options::add, ec);
#endif
        } else {
            cerr << "[Error] Failed to generate executable (llc/clang)." << endl;
        }
    }

    // ลบไฟล์ .ll ตัวกลางทิ้ง เว้นแต่ผู้ใช้ขอเก็บไว้ด้วย -keep-ir=true
    if (!ConfigMap["-keep-ir"].BoolValue) {
        std::error_code ec;
        fs::remove(irPath, ec);
    }
}

int main(int argc, char* args[]) {
    try {
        exePath = fs::weakly_canonical(fs::absolute(fs::path(args[0]))).parent_path().parent_path();
    } catch (const std::exception& e) {
        exePath = fs::current_path();
    }
    WalkRulePath = fs::path(exePath) / "ThaiThonRule.json";

    ReadConfigFile();          // 1) โหลดค่าเริ่มต้นจากไฟล์
    configMapping(args, argc); // 2) override ด้วย command line

    if (ConfigMap["debug"].BoolValue) {
        ShowLogo();
        PrintConfig();
    }

    RunCompiler();
    return 0;
}