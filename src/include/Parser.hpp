#ifndef PARSER
#define PARSER
#include <iostream>
#include <random>       // std::random_device / mt19937 / uniform_int_distribution
#include <algorithm>     // std::find
#include <sstream>       // ostringstream (used when building multi-arg call text)
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include "Executor.hpp"
#include "Mono_c11.hpp"
#include "nlohmann/json.hpp"

using namespace std;
namespace fs = std::filesystem;
using json = nlohmann::json;

// NOTE / ASSUMPTIONS (Mono_c11.hpp / Token were not shown to me, so I'm
// assuming the shape implied by the original code):
//   struct Token {
//       string type;   // e.g. "IDENTIFIER", "STRING", "DOT", "LPAREN", ...
//       json   value;  // literal payload (string/number) or identifier text
//       int    line;
//       int    col;
//   };
// If your real Token differs, adjust current().value.get<string>() calls
// below accordingly.

// ─────────────────────────────────────────────────────────────────────────
//  C / C++ FFI SUPPORT  (new)
//
//  Goal: let ThaiThon code call functions written in plain C or C++,
//  without touching the lexer/grammar at all. Everything is layered on
//  top of the `import "name";`  /  `name.func(args);` syntax that already
//  existed for the built-in stdio-style libraries.
//
//  lib_header.json now understands two extra, OPTIONAL keys per library:
//
//    {
//      "mymath": {
//        "source": ["mymath.c"],                 // NEW: files to compile+link
//        "declare": ["declare i32 @add_numbers(i32, i32)"],
//        "functions": {
//          "add": {
//            "symbol": "add_numbers",
//            "returns": "i32",
//            "params": ["i32", "i32"]             // NEW: real argument types
//          }
//        }
//      }
//    }
//
//  - "source" is a list of .c/.cpp files (resolved relative to the folder
//    that contains lib_header.json, i.e. MotherPath, unless the path is
//    already absolute; for entries coming from a project's own lib.json,
//    see below, they resolve relative to THAT file's folder instead).
//    When any function from this library is imported, Parser remembers
//    these paths in `externSources` so main.cpp can compile them with
//    clang and link the resulting .o files into the final executable
//    (only wired up for `-target=exe` — see main.cpp).
//
//  - "params" is the ordered list of LLVM argument types the target C/C++
//    function actually expects. If present, callStatement() type-checks
//    and lowers every call argument accordingly (currently: integer/float
//    literals -> "i32"/"i64"/"double" etc., string literals -> interned
//    "ptr" global). If a library entry has NO "params" (e.g. the original
//    stdio-only lib_header.json), the old single-string-argument behaviour
//    is used unchanged, so existing lib_header.json files keep working.
//
//  Caveats (by design, to keep this change small and self-contained):
//    * Call arguments must be literals (INT/FLOAT/STRING tokens) — passing
//      a ThaiThon *variable* into a linked C function isn't wired up yet,
//      since this Parser doesn't currently track variable -> LLVM register
//      mappings (that machinery lives over in mono.hpp's Normalizer, which
//      is a separate pipeline from this hand-rolled Parser).
//    * If your "source" file is C++ (.cpp), wrap the linked functions in
//      `extern "C" { ... }` so their symbol names aren't name-mangled —
//      the "symbol" in lib_header.json is looked up as a plain C symbol.
//
//  Also new: a project can drop its own "lib.json" (same schema) next to
//  its .tt file or in the folder it's compiled from, instead of editing
//  the compiler's shared lib_header.json every time. See
//  mergeProjectLibFile() / resolveLibSourcesInPlace() below.
// ─────────────────────────────────────────────────────────────────────────

class Parser {
private:
    int pc = 0;
    vector<string> program;
    vector<string> import;
    vector<string> variable;
    vector<string> function;
    vector<Token> tokens;
    fs::path MotherPath;
    json lib_header;

    // --- LLVM IR accumulators -------------------------------------------
    vector<string> llvmDeclares;   // e.g. "declare i32 @puts(ptr)"
    vector<string> llvmGlobals;    // string constants
    vector<string> llvmBody;       // instructions inside @main
    int stringCounter = 0;

    // NEW: C/C++ source files that need to be compiled and linked in,
    // collected from every imported library's "source" list. Exposed via
    // getExternSources() so main.cpp's link step can see them.
    vector<fs::path> externSources;

    // NEW: system libraries (e.g. "m" for libm, needed by sqrt/pow/etc.)
    // that the linker needs -l<name> for, collected from every imported
    // library's "libs" list. Exposed via getExternLibs().
    vector<string> externLibs;

    Token current() {
        return this->tokens[this->pc];
    }
    void advance() {
        this->pc += 1;
    }
    void advance(int k) {
        this->pc += k;
    }
    void expart_type(string type, string content) {
        if (this->current().type != type) {
            cout << "[Error] from " << type << " " << content << " "
                 << this->current().line << ":" << this->current().col << endl;
            exit(1);
        }
        return;
    }
    void expart_value(string value, string content) {
        if (this->current().value != value) {
            cout << "[Error] " << content << " " << this->current().line << ":"
                 << this->current().col << endl;
            exit(1);
        }
        return;
    }
    void error(string content) {
        cout << "[Error] " << content << " " << this->current().line << ":"
             << this->current().col << endl;
        exit(1);
    }
    std::string generateRandomString(size_t length) {
        const std::string chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        std::random_device rd;
        std::mt19937 generator(rd());
        std::uniform_int_distribution<> dist(0, chars.size() - 1);

        std::string rand_str;
        for (size_t i = 0; i < length; ++i) {
            rand_str += chars[dist(generator)];
        }
        return rand_str;
    }

    // Collects every token between '(' and ')' into `tokens`, then requires
    // a trailing ';'.
    void parenStatement(vector<Token> &tokens) {
        this->expart_type("LPAREN", "Expected '(' after function name");
        this->advance();
        while (this->current().type != "RPAREN") {
            if (this->current().type == "SEMICOLON") {
                this->error("SyntaxError: Expected ')'");
            }
            tokens.push_back(this->current());
            this->advance();
        }
        this->advance();
        this->expart_type("SEMICOLON", "SyntaxError: Expected ';' after function call");
        this->advance();
        return;
    }

    // Splits a flat token list on COMMA into a list of argument token lists.
    vector<vector<Token>> splitArgs(vector<Token> &tokens) {
        vector<vector<Token>> args;
        vector<Token> cur;
        for (auto &t : tokens) {
            if (t.type == "COMMA") {
                args.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(t);
            }
        }
        if (!cur.empty()) args.push_back(cur);
        return args;
    }

    // Very small LLVM string-escaper: escapes '"' and '\' and appends the
    // mandatory NUL terminator LLVM string constants expect.
    string llvmEscape(const string &text) {
        string escaped;
        for (char c : text) {
            if (c == '"' || c == '\\') escaped += '\\';
            escaped += c;
        }
        return escaped;
    }

    // NEW: interns a string literal as a private global constant and
    // returns its "@.str.N" pointer label. Shared by both the typed
    // (params-driven) call path and the legacy single-string call path so
    // there's only one place that formats LLVM string globals.
    string internStringLiteral(const string &text) {
        string label = "@.str." + to_string(this->stringCounter++);
        string escaped = this->llvmEscape(text);
        int len = (int)text.size() + 1; // +1 for the NUL terminator
        this->llvmGlobals.push_back(
            label + " = private unnamed_addr constant [" + to_string(len) +
            " x i8] c\"" + escaped + "\\00\"");
        return label;
    }

    // NEW: lowers a single call argument (a small token group, currently
    // expected to be exactly one literal token) into a fully-formed LLVM
    // operand string such as "i32 3" or "ptr @.str.0", according to the
    // expected `paramType` declared in lib_header.json's "params".
    string lowerArgToLLVM(const string &paramType, vector<Token> &argTokens,
                           const string &funcName) {
        if (argTokens.empty()) {
            this->error("'" + funcName + "' called with a missing argument");
        }
        if (argTokens.size() > 1) {
            this->error("'" + funcName + "': arguments must currently be a single literal "
                        "(int/float/string) — expressions and variables aren't supported yet");
        }
        Token &t = argTokens[0];

        if (paramType == "ptr") {
            if (t.type != "STRING") {
                this->error("'" + funcName + "' expects a string literal for a 'ptr' argument");
            }
            string label = this->internStringLiteral(t.value.get<string>());
            return "ptr " + label;
        }

        if (paramType == "i1" || paramType == "i8" || paramType == "i16" ||
            paramType == "i32" || paramType == "i64") {
            if (t.type != "INT") {
                this->error("'" + funcName + "' expects an integer literal for a '" + paramType + "' argument");
            }
            return paramType + " " + t.value.dump();
        }

        if (paramType == "float" || paramType == "double") {
            if (t.type != "FLOAT" && t.type != "INT") {
                this->error("'" + funcName + "' expects a numeric literal for a '" + paramType + "' argument");
            }
            // NOTE: LLVM textual IR requires a decimal point in a float/double
            // constant, otherwise it's parsed as an *integer* constant (which
            // is a type mismatch against a "double"/"float" declared param --
            // this is exactly the "integer constant must have integer type"
            // error llc throws). Plain `ostringstream << double` drops the
            // decimal point for whole numbers (16.0 -> "16"), so we force it
            // with std::showpoint instead of hand-appending ".0" (which only
            // covered the promoted-INT branch, not literal FLOATs like 16.0).
            ostringstream num;
            if (t.type == "FLOAT") num << std::showpoint << t.value.get<double>();
            else num << t.value.get<long long>() << ".0"; // promote INT literal to float text
            return paramType + " " + num.str();
        }

        this->error("'" + funcName + "': unsupported param type '" + paramType +
                    "' declared in lib_header.json (expected ptr / i1 / i8 / i16 / i32 / i64 / float / double)");
        return ""; // unreachable, error() exits
    }

    void bracketStatement() {

    }

    void braceStatement() {

    }

    // Parses `import "name";` — validates the library exists in
    // lib_header.json, pulls in its LLVM `declare` lines, and (NEW) any
    // C/C++ "source" files it needs linked in.
    void importStatement() {
        this->advance();
        this->expart_type("STRING", "import name is not STRING type");
        string name = this->current().value.get<string>();
        this->advance();
        this->expart_type("SEMICOLON", "Expected ';' after import statement");
        this->advance();

        if (!this->lib_header.contains(name)) {
            this->error("Unknown library '" + name + "'");
        }
        this->import.push_back(name);

        if (this->lib_header[name].contains("declare")) {
            for (auto &d : this->lib_header[name]["declare"]) {
                string decl = d.get<string>();
                if (find(this->llvmDeclares.begin(), this->llvmDeclares.end(), decl)
                    == this->llvmDeclares.end()) {
                    this->llvmDeclares.push_back(decl);
                }
            }
        }

        // NEW: register any C/C++ source files this library needs linked
        // in. Relative paths are resolved against MotherPath (the folder
        // that holds lib_header.json), so a project layout like
        //   ThaiThon/lib_header.json
        //   ThaiThon/libs/mymath.c
        // can just write "source": ["libs/mymath.c"].
        if (this->lib_header[name].contains("source")) {
            for (auto &s : this->lib_header[name]["source"]) {
                fs::path srcPath = s.get<string>();
                if (srcPath.is_relative()) srcPath = this->MotherPath / srcPath;
                if (find(this->externSources.begin(), this->externSources.end(), srcPath)
                    == this->externSources.end()) {
                    this->externSources.push_back(srcPath);
                }
            }
        }

        // NEW: register any system libraries this library needs at link
        // time, e.g. "libs": ["m"] for libm (sqrt/pow/...), "libs": ["pthread"]
        // for POSIX threads. main.cpp turns each entry into a "-l<name>" flag.
        if (this->lib_header[name].contains("libs")) {
            for (auto &l : this->lib_header[name]["libs"]) {
                string libName = l.get<string>();
                if (find(this->externLibs.begin(), this->externLibs.end(), libName)
                    == this->externLibs.end()) {
                    this->externLibs.push_back(libName);
                }
            }
        }
    }

    // Handles calls of the form  module.function(args...);
    // e.g.  stdio.print("Hello World");   or   mymath.add(3, 4);
    void callStatement() {
        string moduleName = this->current().value.get<string>();
        int callLine = this->current().line;
        int callCol = this->current().col;
        this->advance();

        this->expart_type("DOT", "Expected '.' after module name '" + moduleName + "'");
        this->advance();

        // After a DOT we're in "member access" context, so the next token
        // being lexed as a *keyword* type (e.g. "print") shouldn't
        // disqualify it from being a valid function name — only
        // structural/punctuation/literal tokens should.
        static const vector<string> nonNameTypes = {
            "LPAREN", "RPAREN", "LBRACE", "RBRACE", "LBRACKET", "RBRACKET",
            "SEMICOLON", "COMMA", "COLON", "DOT", "EOF", "NEWLINE",
            "STRING", "INT", "FLOAT", "CHAR", "BOOL",
            "PLUS", "MINUS", "MUL", "DIV", "ASSIGN", "EQ", "NEQ", "GT", "LT", "GTE", "LTE"
        };
        if (!this->current().value.is_string() ||
            find(nonNameTypes.begin(), nonNameTypes.end(), this->current().type) != nonNameTypes.end()) {
            this->error("Expected function name after '" + moduleName + ".'");
        }
        string funcName = this->current().value.get<string>();
        this->advance();

        if (find(this->import.begin(), this->import.end(), moduleName) == this->import.end()) {
            this->error("Module '" + moduleName + "' is not imported");
        }
        if (!this->lib_header.contains(moduleName) ||
            !this->lib_header[moduleName].contains("functions") ||
            !this->lib_header[moduleName]["functions"].contains(funcName)) {
            this->error("Unknown function '" + funcName + "' in module '" + moduleName + "'");
        }

        vector<Token> rawArgs;
        this->parenStatement(rawArgs);
        vector<vector<Token>> args = this->splitArgs(rawArgs);

        json funcInfo = this->lib_header[moduleName]["functions"][funcName];
        string symbol  = funcInfo["symbol"].get<string>();
        string retType = funcInfo.value("returns", "i32");

        // NEW: typed multi-argument call path, driven by an explicit
        // "params" array in lib_header.json. This is what makes linking a
        // real C/C++ function (that takes ints/floats/multiple args) work.
        if (funcInfo.contains("params") && funcInfo["params"].is_array()) {
            vector<string> paramTypes;
            for (auto &p : funcInfo["params"]) paramTypes.push_back(p.get<string>());

            bool argsEmptyCall = (args.size() == 1 && args[0].empty());
            size_t actualArgCount = (paramTypes.empty() && argsEmptyCall) ? 0 : args.size();

            if (actualArgCount != paramTypes.size()) {
                this->error("'" + funcName + "' expects " + to_string(paramTypes.size()) +
                            " argument(s) but got " + to_string(actualArgCount));
            }

            vector<string> llvmArgs;
            for (size_t i = 0; i < paramTypes.size(); ++i) {
                llvmArgs.push_back(this->lowerArgToLLVM(paramTypes[i], args[i], funcName));
            }

            ostringstream call;
            call << "call " << retType << " @" << symbol << "(";
            for (size_t i = 0; i < llvmArgs.size(); ++i) {
                if (i) call << ", ";
                call << llvmArgs[i];
            }
            call << ")";
            this->llvmBody.push_back(call.str());
            return;
        }

        // LEGACY PATH: library entry declared no "params" (e.g. the
        // original stdio-only lib_header.json) — keep the exact old
        // behaviour so those files don't need to change at all.
        if (args.empty() || args[0].empty() || args[0][0].type != "STRING") {
            this->error("'" + funcName + "' expects a string as its first argument");
        }
        string label = this->internStringLiteral(args[0][0].value.get<string>());
        this->llvmBody.push_back(
            "call " + retType + " @" + symbol + "(ptr " + label + ")");

        if (args.size() > 1) {
            cout << "[Warning] " << funcName
                 << "(): extra arguments ignored (only the string arg is used) at "
                 << callLine << ":" << callCol
                 << " -- add a \"params\" array to this function in lib_header.json"
                 << " to accept more/typed arguments" << endl;
        }
    }

    void statements() {
        if (this->current().type == "NEWLINE") {
            this->advance();
            return;
        }

        if (this->current().type == "import") {
            this->importStatement();
        } else if (this->current().type == "IDENTIFIER") {
            // A statement starting with an identifier is treated as a
            // `module.function(...)` call (covers both built-in libraries
            // and user-linked C/C++ ones — they go through the same path).
            this->callStatement();
        } else {
            this->error(("Not commands " + this->current().value.dump()));
        }
    }

    // Assembles the collected declares / globals / body into a .ll file.
    void emitLLVM(fs::path outputPath) {
        ofstream out(outputPath);
        if (!out.is_open()) {
            cout << "[Error] Cannot open output LLVM IR file: " << outputPath << endl;
            exit(1);
        }
        for (auto &d : this->llvmDeclares) out << d << "\n";
        out << "\n";
        for (auto &g : this->llvmGlobals) out << g << "\n";
        out << "\n";
        out << "define i32 @main() {\n";
        out << "entry:\n";
        for (auto &b : this->llvmBody) out << "  " << b << "\n";
        out << "  ret i32 0\n";
        out << "}\n";
        out.close();
    }

    // ─────────────────────────────────────────────────────────────────
    //  PROJECT-LOCAL lib.json  (new)
    //
    //  Goal: let a project declare/link its own C/C++ functions in a
    //  small "lib.json" that lives WITH the project, instead of editing
    //  the compiler's shared lib_header.json (in MotherPath) every time.
    //  lib.json uses the exact same schema as lib_header.json (library ->
    //  {source, libs, declare, functions}), and gets merged on top of it
    //  using JSON Merge Patch semantics (RFC 7386): fields the project
    //  doesn't mention are left alone; fields it does mention win. So a
    //  project can add a brand-new library, or just add one more function
    //  to an existing library, without disturbing anything else.
    // ─────────────────────────────────────────────────────────────────

    // Rewrites every "source" path in libSet (for every library entry)
    // from relative-to-baseDir into an absolute path. Called on a
    // project's lib.json right after loading it, BEFORE merging it into
    // the shared lib_header — that way importStatement()'s existing
    // "prefix with MotherPath if relative" logic just leaves these alone
    // (they're already absolute), so project sources resolve relative to
    // the project's lib.json instead of the compiler's folder.
    void resolveLibSourcesInPlace(json &libSet, const fs::path &baseDir) {
        for (auto &item : libSet.items()) {
            json &libEntry = item.value();
            if (!libEntry.is_object() || !libEntry.contains("source")) continue;
            for (auto &s : libEntry["source"]) {
                if (!s.is_string()) continue;
                fs::path p = s.get<string>();
                if (p.is_relative()) {
                    s = (baseDir / p).lexically_normal().string();
                }
            }
        }
    }

    // Looks for "lib.json" inside `dir` and, if found, merges it on top
    // of this->lib_header (project entries win). Safe to call more than
    // once with different directories (e.g. once for the current working
    // directory, once for the folder holding the .tt file being compiled).
    void mergeProjectLibFile(const fs::path &dir) {
        if (dir.empty()) return;
        fs::path projectLibPath = dir / "lib.json";
        if (!fs::exists(projectLibPath)) return;

        ifstream projFile(projectLibPath);
        if (!projFile.is_open()) {
            cout << "[Warning] Found " << projectLibPath << " but couldn't open it; skipping." << endl;
            return;
        }

        json projectLib;
        try {
            projFile >> projectLib;
        } catch (const json::parse_error &e) {
            cout << "[Error] Failed to parse " << projectLibPath << ": " << e.what() << endl;
            exit(1);
        }

        this->resolveLibSourcesInPlace(projectLib, dir);
        this->lib_header.merge_patch(projectLib);
        cout << "[Info] Linked project lib file: " << projectLibPath << endl;
    }

public:
    // `_projectDir` is optional (defaults to empty = "don't look anywhere
    // extra") so any existing 2-argument call sites keep compiling as-is.
    // main.cpp passes the folder containing the input .tt file here.
    Parser(vector<Token> _tokens, fs::path _path, fs::path _projectDir = fs::path()) {
        this->tokens = _tokens;
        this->MotherPath = _path;

        fs::path libPath = this->MotherPath / "lib_header.json";
        ifstream libfile(libPath);
        if (!libfile.is_open()) {
            cout << "[Error] can't open lib header file please check in ThaiThon: "
                 << libPath << endl;
            exit(1);
        }

        libfile >> this->lib_header;

        // NEW: merge in an optional project-local lib.json. Checked in
        // both the current working directory (where the compiler is
        // usually invoked from) and the input file's own directory (in
        // case it differs, e.g. `-i=./test/program.tt`), so either place
        // works without any extra flag. If both exist, the input file's
        // directory wins for any overlapping keys (merged last).
        fs::path cwd = fs::current_path();
        this->mergeProjectLibFile(cwd);
        if (!_projectDir.empty() && fs::absolute(_projectDir) != fs::absolute(cwd)) {
            this->mergeProjectLibFile(_projectDir);
        }
    }

    void run() {
        while (this->current().type != "EOF") {
            this->statements();
        }
    }

    // Parse + emit LLVM IR to a .ll file in one call.
    void run(fs::path outputLLVMPath) {
        this->run();
        this->emitLLVM(outputLLVMPath);
    }

    // NEW: exposes the C/C++ source files collected from imported
    // libraries' "source" lists, so main.cpp can compile+link them into
    // the final executable.
    const vector<fs::path>& getExternSources() const {
        return this->externSources;
    }

    // NEW: exposes the system libraries (e.g. "m") collected from imported
    // libraries' "libs" lists, so main.cpp can pass "-l<name>" to the linker.
    const vector<string>& getExternLibs() const {
        return this->externLibs;
    }
};

#endif