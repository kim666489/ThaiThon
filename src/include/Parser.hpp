#ifndef PARSER
#define PARSER
#include <iostream>
#include <random>
#include <algorithm>
#include <sstream>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <set>
#include <unordered_map>
#include <map>
#include "Executor.hpp"
#include "Mono_c11.hpp"
#include "nlohmann/json.hpp"
#include "ModuleWriter.hpp"

using namespace std;
namespace fs = std::filesystem;
using json = nlohmann::json;

// ─────────────────────────────────────────────────────────────────────────
//  Parser.hpp  —  v2
//
//  What changed from v1 (the FFI-only version):
//    - v1 could only parse `import "lib";` and `module.function(args);`.
//      All codegen bookkeeping (declares/globals/body/string interning)
//      lived directly inside this class.
//    - v2 adds a real (if intentionally small) language surface on top of
//      that: `let` / `const` (typed or auto), `if` / `else if` / `else`,
//      `while`, `function`, `class`, `return`, arithmetic + comparison
//      expressions, user-function calls, and a set of built-ins (print/
//      println, input, split/arraylen/arrayget, readfile/writefile,
//      json*) implemented by Runtime.cpp. All LLVM text assembly is now
//      delegated to ModuleWriter (see ModuleWriter.hpp) so this class only
//      has to decide *what* IR to emit, not how to format/dedupe it.
//
//  Design choices worth knowing about before you extend this further —
//  see README.md's "How the compiler works" section for the long version:
//    - No SSA / mem2reg: every variable is an `alloca` stack slot, loaded/
//      stored on every use. Valid, llc-friendly LLVM IR, just not
//      optimal — pipe the output through `-opt=O1` (main.cpp already
//      supports this) if that matters to you.
//    - "float" and "double" are the same LLVM type (`double`) internally.
//    - Strings/json handles/arrays/class instances are all the opaque
//      LLVM `ptr` type at the machine level; ThaiThon's static type
//      (string/json/array/ClassName) is tracked separately, only inside
//      the parser, purely for type-checking and for picking the right
//      built-in overload (e.g. which tt_print_* to call).
//    - Declare functions and classes before you use them (top-to-bottom,
//      single pass — like most scripting languages, unlike C).
//    - No closures, no inheritance/vtables/methods on `class` (fields
//      only — write free `function`s that take the object as their first
//      parameter instead), no `[]` array-literal syntax (use split() /
//      arraylen() / arrayget()), no `&&` / `||` (one comparison per
//      `if`/`while` condition).
// ─────────────────────────────────────────────────────────────────────────

class Parser {
private:
    int pc = 0;
    vector<string> import;
    vector<Token> tokens;
    fs::path RootPath;
    fs::path MotherPath;
    fs::path ProjectPath;
    json lib_header;

    ModuleWriter writer;

    // NEW: C/C++ source files / system libs collected from imported
    // libraries, exposed the same way v1 did so main.cpp's link step
    // doesn't need to change.
    vector<fs::path> externSources;
    vector<string> externLibs;

    // NEW: linked ThaiThon source files, to support programs split across
    // multiple .tt files.
    vector<fs::path> linkedFiles;

    // ── language-level symbol tables ────────────────────────────────────

    struct VarInfo {
        string llvmType;   // "i32" | "double" | "i1" | "i8" | "ptr"
        string reg;        // alloca'd stack-slot register, e.g. "%x_3"
        bool isConst = false;
        string className;  // non-empty only if llvmType=="ptr" and it's a class instance
    };
    // Stack of block scopes for whichever function we're currently
    // generating code into. Saved/restored around function bodies since
    // ThaiThon has no closures / nested function defs.
    vector<unordered_map<string, VarInfo>> scopes;
    vector<pair<string, string>> loopLabels; // {continueTarget, endTarget}

    struct ClassDef {
        string name;
        vector<string> fieldTypes;               // llvm types, in declaration order
        vector<string> fieldNames;
        unordered_map<string, int> fieldIndex;
    };
    unordered_map<string, ClassDef> classes;

    struct FuncDef {
        vector<pair<string, string>> params;      // (llvmType, name)
        string retType;                           // llvm type, or "void"
    };
    unordered_map<string, FuncDef> functions;

    string currentRetType = "i32"; // return type of the function currently being generated (main == i32)

    struct BuiltinSig {
        string symbol;
        string retType;                 // llvm type ("void" allowed)
        vector<string> paramTypes;      // llvm types
        string declareLine;
    };
    unordered_map<string, BuiltinSig> builtins;

    struct ExprResult {
        string type;       // llvm type
        string value;      // operand text: "%t3", "3", "@.str.0", ...
        string className;  // non-empty only for ptr values that are class instances
    };

    // ── token plumbing ───────────────────────────────────────────────────

    Token current() { return this->tokens[this->pc]; }
    Token peekAt(int k) {
        int idx = this->pc + k;
        if (idx < 0) idx = 0;
        if (idx >= (int)this->tokens.size()) idx = (int)this->tokens.size() - 1;
        return this->tokens[idx];
    }
    void advance() { this->pc += 1; }
    void advance(int k) { this->pc += k; }

    void expart_type(string type, string content) {
        if (this->current().type != type) {
            cout << "[Error] from " << type << " " << content << " "
                 << this->current().line << ":" << this->current().col << endl;
            exit(1);
        }
    }
    [[noreturn]] void error(string content) {
        cout << "[Error] " << content << " " << this->current().line << ":"
             << this->current().col << endl;
        exit(1);
    }

    static bool currentIsValue(Token& t, const string& v) {
        return t.value.is_string() && t.value.get<string>() == v;
    }
    bool atKeyword(const string& v) {
        Token t = this->current();
        if (t.value.is_string() && t.value.get<string>() == v) return true;
        return t.type == v;
    }

    // ── type helpers ─────────────────────────────────────────────────────

    static bool isTypeKeyword(const string& v) {
        static const set<string> kw = {"int", "float", "double", "string", "bool", "char", "json", "void", "auto"};
        return kw.count(v) != 0;
    }

    string mapDeclaredType(const string& t) {
        if (t == "int") return "i32";
        if (t == "float" || t == "double") return "double";
        if (t == "string") return "ptr";
        if (t == "bool") return "i1";
        if (t == "char") return "i8";
        if (t == "json") return "ptr";
        if (t == "void") return "void";
        if (this->classes.count(t)) return "ptr";
        this->error("Unknown type '" + t + "'");
    }

    static string defaultValueFor(const string& llvmType) {
        if (llvmType == "double") return "0.0";
        if (llvmType == "ptr") return "null";
        if (llvmType == "i1") return "0";
        return "0"; // i8/i32/i64
    }

    // ── string helpers ───────────────────────────────────────────────────

    string internStringLiteral(const string& text) { return this->writer.internString(text); }

    // ── scope helpers ────────────────────────────────────────────────────

    void pushScope() { this->scopes.push_back({}); }
    void popScope() { this->scopes.pop_back(); }

    void declareVar(const string& name, const string& llvmType, const string& reg,
                     bool isConst, const string& className = "") {
        this->scopes.back()[name] = VarInfo{llvmType, reg, isConst, className};
    }
    VarInfo* findVar(const string& name) {
        for (auto it = this->scopes.rbegin(); it != this->scopes.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

public:
    Parser(vector<Token> _tokens, fs::path _rootPath, fs::path _projectDir = fs::path())
        : writer("thaithon_module") {
        this->tokens = _tokens;
        this->RootPath = fs::absolute(_rootPath).lexically_normal();
        this->MotherPath = this->RootPath; // compiler-owned resources stay rooted here
        this->ProjectPath = _projectDir.empty() ? this->RootPath : fs::absolute(_projectDir).lexically_normal();

        fs::path libPath = this->MotherPath / "lib_header.json";
        ifstream libfile(libPath);
        if (!libfile.is_open()) {
            cout << "[Error] can't open lib header file please check in ThaiThon: " << libPath << endl;
            exit(1);
        }
        libfile >> this->lib_header;

        // User project files and compiler-owned files must not share the same search scope.
        // Only the active project directory is considered for project-local lib.json merging.
        if (!this->ProjectPath.empty()) {
            this->mergeProjectLibFile(this->ProjectPath);
        }

        this->registerBuiltins();
        this->scopes.push_back({}); // global/@main scope
    }

    void run() {
        while (this->current().type != "EOF") {
            this->statements();
        }
        // close out @main with a default `ret i32 0` if the program never
        // explicitly returned.
        this->writer.setCurrentFunction(this->writer.mainFunctionIndex());
        if (!this->writer.lastIsTerminator()) this->writer.emit("ret i32 0");
    }

    void run(fs::path outputLLVMPath) {
        this->run();
        this->writer.write(outputLLVMPath);
    }

    const vector<fs::path>& getExternSources() const { return this->externSources; }
    const vector<string>& getExternLibs() const { return this->externLibs; }

private:
    // ─────────────────────────────────────────────────────────────────
    //  BUILT-INS  (backed by Runtime.cpp — see that file for behaviour)
    // ─────────────────────────────────────────────────────────────────
    void registerBuiltins() {
        auto reg = [&](string name, string symbol, string ret, vector<string> params) {
            ostringstream d;
            d << "declare " << ret << " @" << symbol << "(";
            for (size_t i = 0; i < params.size(); ++i) { if (i) d << ", "; d << params[i]; }
            d << ")";
            this->builtins[name] = BuiltinSig{symbol, ret, params, d.str()};
        };
        // string/i/o
        reg("input", "tt_input", "ptr", {"ptr"});
        reg("readfile", "tt_read_file", "ptr", {"ptr"});
        reg("writefile", "tt_write_file", "i64", {"ptr", "ptr"});
        reg("strconcat", "tt_str_concat", "ptr", {"ptr", "ptr"});
        reg("streq", "tt_str_eq", "i8", {"ptr", "ptr"});
        reg("strlen", "tt_str_len", "i64", {"ptr"});
        // split / array
        reg("split", "tt_split", "ptr", {"ptr", "ptr"});
        reg("arraylen", "tt_array_len", "i64", {"ptr"});
        reg("arrayget", "tt_array_get", "ptr", {"ptr", "i64"});
        // json
        reg("jsonparse", "tt_json_parse", "ptr", {"ptr"});
        reg("jsonreadfile", "tt_json_read_file", "ptr", {"ptr"});
        reg("jsonwritefile", "tt_json_write_file", "i64", {"ptr", "ptr"});
        reg("jsonstringify", "tt_json_stringify", "ptr", {"ptr"});
        reg("jsonnew", "tt_json_new_object", "ptr", {});
        reg("jsonsetstring", "tt_json_set_string", "void", {"ptr", "ptr", "ptr"});
        reg("jsonsetint", "tt_json_set_int", "void", {"ptr", "ptr", "i64"});
        reg("jsonsetdouble", "tt_json_set_double", "void", {"ptr", "ptr", "double"});
        reg("jsonsetbool", "tt_json_set_bool", "void", {"ptr", "ptr", "i8"});
        reg("jsongetstring", "tt_json_get_string", "ptr", {"ptr", "ptr"});
        reg("jsongetint", "tt_json_get_int", "i64", {"ptr", "ptr"});
        reg("jsongetdouble", "tt_json_get_double", "double", {"ptr", "ptr"});
        reg("jsongetbool", "tt_json_get_bool", "i8", {"ptr", "ptr"});
        reg("jsongetobject", "tt_json_get_object", "ptr", {"ptr", "ptr"});
        reg("jsonhas", "tt_json_has", "i8", {"ptr", "ptr"});
        // print/println are handled specially (polymorphic on arg type),
        // not through this generic table — see callPrint().
    }

    // ─────────────────────────────────────────────────────────────────
    //  STATEMENT DISPATCH
    // ─────────────────────────────────────────────────────────────────
    void statements() {
        Token t = this->current();

        if (t.type == "NEWLINE") { this->advance(); return; }
        if (t.type == "HASH") { this->includeStatement(); return; }

        if (t.type == "import" || this->atKeyword("import")) { this->importStatement(); return; }
        if (this->atKeyword("include")) { this->includeStatement(); return; }
        if (this->atKeyword("link")) { this->linkStatement(); return; }
        if (this->atKeyword("let")) { this->letStmt(false); return; }
        if (this->atKeyword("const")) { this->letStmt(true); return; }
        if (this->atKeyword("if")) { this->ifStmt(); return; }
        if (this->atKeyword("else")) { this->error("'else' must follow an 'if' block"); }
        if (this->atKeyword("while")) { this->whileStmt(); return; }
        if (this->atKeyword("function")) { this->funcDecl(); return; }
        if (this->atKeyword("class")) { this->classDecl(); return; }
        if (this->atKeyword("return")) { this->returnStmt(); return; }
        if (this->atKeyword("continue")) { this->continueStmt(); return; }
        if (this->atKeyword("pass")) { this->passStmt(); return; }
        if (this->atKeyword("print")) { this->callPrint(false); this->expectSemi(); return; }
        if (this->atKeyword("println")) { this->callPrint(true); this->expectSemi(); return; }
        if (this->current().type == "COMMENT" || this->current().value == "//" || this->current().value == "/*") { this->advance(); return; }

        if (t.type == "IDENTIFIER") {
            Token n1 = this->peekAt(1);
            if (n1.type == "DOT") {
                string moduleName = t.value.get<string>();
                if (this->findVar(moduleName)) {
                    this->fieldAssignStatement();
                    return;
                }
                // otherwise: imported-library call, e.g. stdio.print("hi");
                this->callStatement();
                return;
            }
            if (n1.type == "SCOPE") {
                this->callStatement();
                return;
            }
            if (n1.type == "LPAREN") {
                // bare call used as a statement: foo(1,2); or print/println
                // handled above, so this is a user-defined or builtin call.
                this->parseExpr();
                this->expectSemi();
                return;
            }
            if (n1.type == "ASSIGN") {
                this->assignStmt();
                return;
            }
        }

        this->error(("Not commands " + t.value.dump()));
    }

    void expectSemi() {
        this->expart_type("SEMICOLON", "SyntaxError: Expected ';'");
        this->advance();
    }

    void block() {
        this->expart_type("LBRACE", "Expected '{'");
        this->advance();
        while (this->current().type != "RBRACE") {
            if (this->current().type == "EOF") this->error("SyntaxError: unterminated block, expected '}'");
            this->statements();
        }
        this->advance(); // consume '}'
    }

    // ─────────────────────────────────────────────────────────────────
    //  comments
    // ─────────────────────────────────────────────────────────────────
    void passStmt() {
        this->advance();
        this->expectSemi();
    }

    void continueStmt() {
        this->advance();
        this->expectSemi();
        if (this->loopLabels.empty()) this->error("'continue' can only be used inside a loop");
        this->writer.emit("br label %" + this->loopLabels.back().first);
    }

    // ─────────────────────────────────────────────────────────────────
    //  let / const
    // ─────────────────────────────────────────────────────────────────
    void letStmt(bool isConst) {
        this->advance(); // 'let' / 'const'

        string declaredType = "";
        if (this->current().type == "IDENTIFIER") {
            string v = this->current().value.get<string>();
            bool looksLikeType = isTypeKeyword(v) || this->classes.count(v);
            if (looksLikeType && this->peekAt(1).type == "IDENTIFIER") {
                declaredType = v;
                this->advance();
            }
        }

        this->expart_type("IDENTIFIER", "Expected variable name");
        string varName = this->current().value.get<string>();
        if (declaredType.empty() && isTypeKeyword(varName)) {
            this->error("'" + varName + "' is a reserved type name and can't be used as a variable name");
        }
        this->advance();
        this->expart_type("ASSIGN", "Expected '=' after variable name in declaration");
        this->advance();

        ExprResult val = this->parseExpr();
        this->expectSemi();

        string finalType;
        if (declaredType.empty() || declaredType == "auto") {
            finalType = val.type;
        } else {
            finalType = this->mapDeclaredType(declaredType);
            val = this->coerce(val, finalType, varName);
        }

        string reg = "%" + varName + "_" + to_string(this->varCounter++);
        this->writer.emit(reg + " = alloca " + finalType);
        this->writer.emit("store " + finalType + " " + val.value + ", ptr " + reg);
        this->declareVar(varName, finalType, reg, isConst, val.className);
    }
    int varCounter = 0;

    void assignStmt() {
        string name = this->current().value.get<string>();
        this->advance();
        this->expart_type("ASSIGN", "Expected '='");
        this->advance();
        VarInfo* vi = this->findVar(name);
        if (!vi) this->error("Undeclared variable '" + name + "'");
        if (vi->isConst) this->error("Cannot assign to const '" + name + "'");
        ExprResult val = this->parseExpr();
        this->expectSemi();
        val = this->coerce(val, vi->llvmType, name);
        this->writer.emit("store " + vi->llvmType + " " + val.value + ", ptr " + vi->reg);
        if (vi->llvmType == "ptr") vi->className = val.className;
    }

    // obj.field = expr;
    void fieldAssignStatement() {
        string objName = this->current().value.get<string>();
        this->advance();
        this->expart_type("DOT", "Expected '.'");
        this->advance();
        this->expart_type("IDENTIFIER", "Expected field name");
        string fieldName = this->current().value.get<string>();
        this->advance();
        this->expart_type("ASSIGN", "Expected '=' in field assignment");
        this->advance();

        VarInfo* obj = this->findVar(objName);
        if (!obj) this->error("Undeclared variable '" + objName + "'");
        if (obj->className.empty()) this->error("'" + objName + "' is not a class instance");
        ClassDef& cd = this->classes.at(obj->className);
        if (!cd.fieldIndex.count(fieldName)) this->error("Unknown field '" + fieldName + "' on class '" + obj->className + "'");
        int idx = cd.fieldIndex.at(fieldName);
        string fieldType = cd.fieldTypes[idx];

        ExprResult val = this->parseExpr();
        this->expectSemi();
        val = this->coerce(val, fieldType, objName + "." + fieldName);

        string objPtr = this->writer.newTemp();
        this->writer.emit(objPtr + " = load ptr, ptr " + obj->reg);
        string fieldPtr = this->writer.newTemp();
        this->writer.emit(fieldPtr + " = getelementptr %" + obj->className + ", ptr " + objPtr +
                           ", i32 0, i32 " + to_string(idx));
        this->writer.emit("store " + fieldType + " " + val.value + ", ptr " + fieldPtr);
    }

    // ─────────────────────────────────────────────────────────────────
    //  if / else if / else
    // ─────────────────────────────────────────────────────────────────
    void ifStmt() {
        this->advance(); // 'if'
        this->expart_type("LPAREN", "Expected '(' after 'if'");
        this->advance();
        ExprResult cond = this->parseExpr();
        cond = this->ensureBool(cond);
        this->expart_type("RPAREN", "Expected ')' to close 'if' condition");
        this->advance();

        string thenL = this->writer.newLabel("if_then");
        string elseL = this->writer.newLabel("if_else");
        string endL = this->writer.newLabel("if_end");

        this->writer.emit("br i1 " + cond.value + ", label %" + thenL + ", label %" + elseL);

        this->writer.emitLabel(thenL);
        this->pushScope();
        this->block();
        this->popScope();
        if (!this->writer.lastIsTerminator()) this->writer.emit("br label %" + endL);

        this->writer.emitLabel(elseL);
        if (this->atKeyword("else")) {
            this->advance();
            if (this->atKeyword("if")) {
                this->ifStmt(); // recursive: handles its own then/else/end
            } else {
                this->pushScope();
                this->block();
                this->popScope();
            }
        }
        if (!this->writer.lastIsTerminator()) this->writer.emit("br label %" + endL);

        this->writer.emitLabel(endL);
    }

    // ─────────────────────────────────────────────────────────────────
    //  while
    // ─────────────────────────────────────────────────────────────────
    void whileStmt() {
        this->advance(); // 'while'
        this->expart_type("LPAREN", "Expected '(' after 'while'");
        this->advance();

        string condL = this->writer.newLabel("while_cond");
        string bodyL = this->writer.newLabel("while_body");
        string endL = this->writer.newLabel("while_end");

        this->writer.emit("br label %" + condL);
        this->writer.emitLabel(condL);
        ExprResult cond = this->parseExpr();
        cond = this->ensureBool(cond);
        this->expart_type("RPAREN", "Expected ')' to close 'while' condition");
        this->advance();
        this->writer.emit("br i1 " + cond.value + ", label %" + bodyL + ", label %" + endL);

        this->writer.emitLabel(bodyL);
        this->loopLabels.push_back({condL, endL});
        this->pushScope();
        this->block();
        this->popScope();
        this->loopLabels.pop_back();
        if (!this->writer.lastIsTerminator()) this->writer.emit("br label %" + condL);

        this->writer.emitLabel(endL);
    }

    // ─────────────────────────────────────────────────────────────────
    //  function
    // ─────────────────────────────────────────────────────────────────
    void funcDecl() {
        this->advance(); // 'function'
        this->expart_type("IDENTIFIER", "Expected function name");
        string fname = this->current().value.get<string>();
        if (this->functions.count(fname)) this->error("Function '" + fname + "' already declared");
        this->advance();

        this->expart_type("LPAREN", "Expected '(' after function name");
        this->advance();
        vector<pair<string, string>> params;  // (llvmType, name) — used for the FuncDef signature
        vector<string> paramClassNames;        // parallel array: class name if the param's declared
                                                // type was a `class`, else "" — needed so `obj.field`
                                                // works inside the function body (see below)
        while (this->current().type != "RPAREN") {
            this->expart_type("IDENTIFIER", "Expected parameter type");
            string typeTok = this->current().value.get<string>();
            this->advance();
            this->expart_type("IDENTIFIER", "Expected parameter name");
            string pname = this->current().value.get<string>();
            this->advance();
            params.push_back({this->mapDeclaredType(typeTok), pname});
            paramClassNames.push_back(this->classes.count(typeTok) ? typeTok : "");
            if (this->current().type == "COMMA") this->advance();
        }
        this->advance(); // ')'

        string retType = "void";
        if (this->current().type == "IDENTIFIER") {
            string v = this->current().value.get<string>();
            if (isTypeKeyword(v) || this->classes.count(v)) {
                retType = this->mapDeclaredType(v);
                this->advance();
            }
        }

        this->functions[fname] = FuncDef{params, retType};

        vector<pair<string, string>> llvmParams;
        for (auto& p : params) llvmParams.push_back({p.first, "arg_" + p.second});

        int callerFunc = this->writer.currentFunctionIndex();
        string savedRet = this->currentRetType;
        auto savedScopes = this->scopes;

        this->writer.beginFunction(fname, retType, llvmParams);
        this->currentRetType = retType;
        this->scopes.clear();
        this->scopes.push_back({});

        for (size_t pi = 0; pi < params.size(); ++pi) {
            auto& p = params[pi];
            string reg = "%" + p.second + "_arg" + to_string(this->varCounter++);
            this->writer.emit(reg + " = alloca " + p.first);
            this->writer.emit("store " + p.first + " %arg_" + p.second + ", ptr " + reg);
            this->declareVar(p.second, p.first, reg, false, paramClassNames[pi]);
        }

        this->block();

        if (!this->writer.lastIsTerminator()) {
            if (retType == "void") this->writer.emit("ret void");
            else this->writer.emit("ret " + retType + " " + defaultValueFor(retType));
        }

        this->scopes = savedScopes;
        this->currentRetType = savedRet;
        this->writer.setCurrentFunction(callerFunc);
    }

    void returnStmt() {
        this->advance(); // 'return'
        if (this->current().type == "SEMICOLON") {
            this->advance();
            if (this->currentRetType == "void") this->writer.emit("ret void");
            else this->writer.emit("ret " + this->currentRetType + " " + defaultValueFor(this->currentRetType));
            return;
        }
        ExprResult v = this->parseExpr();
        this->expectSemi();
        if (this->currentRetType == "void") this->error("'return' with a value inside a void function");
        v = this->coerce(v, this->currentRetType, "return value");
        this->writer.emit("ret " + this->currentRetType + " " + v.value);
    }

    // ─────────────────────────────────────────────────────────────────
    //  class  (fields only — see README for why methods aren't in scope)
    // ─────────────────────────────────────────────────────────────────
    void classDecl() {
        this->advance(); // 'class'
        this->expart_type("IDENTIFIER", "Expected class name");
        string cname = this->current().value.get<string>();
        if (this->classes.count(cname)) this->error("Class '" + cname + "' already declared");
        this->advance();

        this->expart_type("LBRACE", "Expected '{' after class name");
        this->advance();

        ClassDef def;
        def.name = cname;
        while (this->current().type != "RBRACE") {
            if (this->current().type == "EOF") this->error("SyntaxError: unterminated class body, expected '}'");
            this->expart_type("IDENTIFIER", "Expected field type");
            string typeTok = this->current().value.get<string>();
            this->advance();
            this->expart_type("IDENTIFIER", "Expected field name");
            string fname = this->current().value.get<string>();
            this->advance();
            this->expectSemi();

            string llvmT = this->mapDeclaredType(typeTok);
            def.fieldIndex[fname] = (int)def.fieldTypes.size();
            def.fieldTypes.push_back(llvmT);
            def.fieldNames.push_back(fname);
        }
        this->advance(); // '}'

        this->classes[cname] = def;
        this->writer.addStruct(cname, def.fieldTypes);
    }

    // ─────────────────────────────────────────────────────────────────
    //  print / println  (polymorphic on argument type)
    // ─────────────────────────────────────────────────────────────────
    void callPrint(bool newline) {
        this->advance(); // 'print' / 'println'
        this->expart_type("LPAREN", "Expected '(' after print");
        this->advance();
        bool first = true;
        while (this->current().type != "RPAREN") {
            if (!first) this->emitPrintValue(ExprResult{"ptr", this->internStringLiteral(" ")});
            ExprResult v = this->parseExpr();
            this->emitPrintValue(v);
            first = false;
            if (this->current().type == "COMMA") this->advance();
        }
        this->advance(); // ')'
        if (newline) {
            this->writer.addDeclare("declare void @tt_print_newline()");
            this->writer.emit("call void @tt_print_newline()");
        }
    }

    void emitPrintValue(ExprResult v) {
        if (v.type == "i32") {
            this->writer.addDeclare("declare void @tt_print_i32(i32)");
            this->writer.emit("call void @tt_print_i32(i32 " + v.value + ")");
        } else if (v.type == "i64") {
            this->writer.addDeclare("declare void @tt_print_i64(i64)");
            this->writer.emit("call void @tt_print_i64(i64 " + v.value + ")");
        } else if (v.type == "double") {
            this->writer.addDeclare("declare void @tt_print_double(double)");
            this->writer.emit("call void @tt_print_double(double " + v.value + ")");
        } else if (v.type == "i1") {
            string z = this->writer.newTemp();
            this->writer.emit(z + " = zext i1 " + v.value + " to i8");
            this->writer.addDeclare("declare void @tt_print_bool(i8)");
            this->writer.emit("call void @tt_print_bool(i8 " + z + ")");
        } else if (v.type == "i8") {
            this->writer.addDeclare("declare void @tt_print_char(i8)");
            this->writer.emit("call void @tt_print_char(i8 " + v.value + ")");
        } else if (v.type == "ptr") {
            this->writer.addDeclare("declare void @tt_print_str(ptr)");
            this->writer.emit("call void @tt_print_str(ptr " + v.value + ")");
        } else {
            this->error("print(): unsupported value type '" + v.type + "'");
        }
    }

    // ─────────────────────────────────────────────────────────────────
    //  EXPRESSIONS  (precedence climbing: comparison > +- > */ > unary > primary)
    // ─────────────────────────────────────────────────────────────────
    ExprResult parseExpr() { return this->parseComparison(); }

    ExprResult parseComparison() {
        ExprResult left = this->parseAdditive();
        static const set<string> cmpOps = {"EQ", "NEQ", "GT", "LT", "GTE", "LTE"};
        while (cmpOps.count(this->current().type)) {
            string op = this->current().type;
            this->advance();
            ExprResult right = this->parseAdditive();
            left = this->emitComparison(op, left, right);
        }
        return left;
    }

    ExprResult parseAdditive() {
        ExprResult left = this->parseMultiplicative();
        while (this->current().type == "PLUS" || this->current().type == "MINUS") {
            string op = this->current().type;
            this->advance();
            ExprResult right = this->parseMultiplicative();
            left = this->emitArith(op, left, right);
        }
        return left;
    }

    ExprResult parseMultiplicative() {
        ExprResult left = this->parseUnary();
        while (this->current().type == "MUL" || this->current().type == "DIV") {
            string op = this->current().type;
            this->advance();
            ExprResult right = this->parseUnary();
            left = this->emitArith(op, left, right);
        }
        return left;
    }

    ExprResult parseUnary() {
        if (this->current().type == "MINUS") {
            this->advance();
            ExprResult v = this->parseUnary();
            if (v.type == "i32") {
                string t = this->writer.newTemp();
                this->writer.emit(t + " = sub i32 0, " + v.value);
                return ExprResult{"i32", t};
            }
            if (v.type == "double") {
                string t = this->writer.newTemp();
                this->writer.emit(t + " = fsub double 0.0, " + v.value);
                return ExprResult{"double", t};
            }
            this->error("unary '-' is only valid on int/float values");
        }
        return this->parsePrimary();
    }

    ExprResult parsePrimary() {
        Token t = this->current();

        if (t.type == "INT") {
            this->advance();
            return ExprResult{"i32", t.value.dump()};
        }
        if (t.type == "FLOAT") {
            ostringstream num;
            num << std::showpoint << t.value.get<double>();
            this->advance();
            return ExprResult{"double", num.str()};
        }
        if (t.type == "STRING") {
            string lbl = this->internStringLiteral(t.value.get<string>());
            this->advance();
            return ExprResult{"ptr", lbl};
        }
        if (t.type == "BOOL") {
            string v = t.value.is_string() ? t.value.get<string>() : (t.value.get<bool>() ? "true" : "false");
            this->advance();
            return ExprResult{"i1", (v == "true" || v == "1") ? "1" : "0"};
        }
        if (t.type == "CHAR") {
            string v = t.value.get<string>();
            this->advance();
            return ExprResult{"i8", to_string((int)(unsigned char)(v.empty() ? '\0' : v[0]))};
        }
        if (t.type == "LPAREN") {
            this->advance();
            ExprResult inner = this->parseExpr();
            this->expart_type("RPAREN", "Expected ')' to close parenthesised expression");
            this->advance();
            return inner;
        }
        if (t.type == "IDENTIFIER") {
            string name = t.value.get<string>();

            if (name == "true" || name == "false") {
                this->advance();
                return ExprResult{"i1", name == "true" ? "1" : "0"};
            }
            if (name == "new") return this->parseNewObject();
            if (name == "print" || name == "println") this->error("'" + name + "' returns nothing and can't be used inside an expression");

            Token n1 = this->peekAt(1);

            if (n1.type == "LPAREN") {
                this->advance(); // consume identifier; '(' still current
                return this->parseCall(name);
            }
            if (n1.type == "DOT") {
                // either a class field read (obj.field) or an imported
                // library call used as an expression (module.function(...))
                if (this->findVar(name)) return this->parseFieldRead(name);
                return this->parseModuleCallExpr(name);
            }
            if (n1.type == "SCOPE") {
                return this->parseModuleCallExpr(name);
            }

            // plain variable load
            this->advance();
            VarInfo* vi = this->findVar(name);
            if (!vi) this->error("Undeclared variable '" + name + "'");
            string reg = this->writer.newTemp();
            this->writer.emit(reg + " = load " + vi->llvmType + ", ptr " + vi->reg);
            return ExprResult{vi->llvmType, reg, vi->className};
        }

        this->error("Unexpected token in expression: " + t.type);
    }

    ExprResult parseNewObject() {
        this->advance(); // 'new'
        this->expart_type("IDENTIFIER", "Expected class name after 'new'");
        string cname = this->current().value.get<string>();
        if (!this->classes.count(cname)) this->error("Unknown class '" + cname + "'");
        this->advance();
        this->expart_type("LPAREN", "Expected '(' after class name (no constructor arguments are supported)");
        this->advance();
        if (this->current().type != "RPAREN") this->error("'new " + cname + "(...)' does not take arguments in this version");
        this->advance(); // ')'

        this->writer.addDeclare("declare ptr @malloc(i64)");
        string szPtr = this->writer.newTemp();
        this->writer.emit(szPtr + " = getelementptr %" + cname + ", ptr null, i32 1");
        string szInt = this->writer.newTemp();
        this->writer.emit(szInt + " = ptrtoint ptr " + szPtr + " to i64");
        string obj = this->writer.newTemp();
        this->writer.emit(obj + " = call ptr @malloc(i64 " + szInt + ")");
        return ExprResult{"ptr", obj, cname};
    }

    ExprResult parseFieldRead(const string& objName) {
        this->advance(); // identifier
        this->expart_type("DOT", "Expected '.'");
        this->advance();
        this->expart_type("IDENTIFIER", "Expected field name");
        string fieldName = this->current().value.get<string>();
        this->advance();

        VarInfo* obj = this->findVar(objName);
        if (!obj || obj->className.empty()) this->error("'" + objName + "' is not a class instance");
        ClassDef& cd = this->classes.at(obj->className);
        if (!cd.fieldIndex.count(fieldName)) this->error("Unknown field '" + fieldName + "' on class '" + obj->className + "'");
        int idx = cd.fieldIndex.at(fieldName);
        string fieldType = cd.fieldTypes[idx];

        string objPtr = this->writer.newTemp();
        this->writer.emit(objPtr + " = load ptr, ptr " + obj->reg);
        string fieldPtr = this->writer.newTemp();
        this->writer.emit(fieldPtr + " = getelementptr %" + obj->className + ", ptr " + objPtr +
                           ", i32 0, i32 " + to_string(idx));
        string val = this->writer.newTemp();
        this->writer.emit(val + " = load " + fieldType + ", ptr " + fieldPtr);
        return ExprResult{fieldType, val};
    }

    // user-defined function call OR built-in call, used as an expression
    ExprResult parseCall(const string& fname) {
        this->expart_type("LPAREN", "Expected '('");
        this->advance();
        vector<ExprResult> args;
        while (this->current().type != "RPAREN") {
            args.push_back(this->parseExpr());
            if (this->current().type == "COMMA") this->advance();
        }
        this->advance(); // ')'

        if (this->functions.count(fname)) {
            FuncDef& fd = this->functions.at(fname);
            if (args.size() != fd.params.size())
                this->error("'" + fname + "' expects " + to_string(fd.params.size()) +
                            " argument(s) but got " + to_string(args.size()));
            ostringstream call;
            string retReg;
            if (fd.retType != "void") { retReg = this->writer.newTemp(); call << retReg << " = "; }
            call << "call " << fd.retType << " @" << fname << "(";
            for (size_t i = 0; i < args.size(); ++i) {
                ExprResult a = this->coerce(args[i], fd.params[i].first, "argument " + to_string(i + 1) + " of " + fname);
                if (i) call << ", ";
                call << fd.params[i].first << " " << a.value;
            }
            call << ")";
            this->writer.emit(call.str());
            return ExprResult{fd.retType, fd.retType == "void" ? "" : retReg};
        }

        if (this->builtins.count(fname)) {
            BuiltinSig& sig = this->builtins.at(fname);
            if (args.size() != sig.paramTypes.size())
                this->error("'" + fname + "' expects " + to_string(sig.paramTypes.size()) +
                            " argument(s) but got " + to_string(args.size()));
            this->writer.addDeclare(sig.declareLine);
            ostringstream call;
            string retReg;
            if (sig.retType != "void") { retReg = this->writer.newTemp(); call << retReg << " = "; }
            call << "call " << sig.retType << " @" << sig.symbol << "(";
            for (size_t i = 0; i < args.size(); ++i) {
                ExprResult a = this->coerce(args[i], sig.paramTypes[i], "argument " + to_string(i + 1) + " of " + fname);
                if (i) call << ", ";
                call << sig.paramTypes[i] << " " << a.value;
            }
            call << ")";
            this->writer.emit(call.str());
            return ExprResult{sig.retType, sig.retType == "void" ? "" : retReg};
        }

        this->error("Unknown function '" + fname + "'");
    }

    // module.function(args) used as an *expression* (has a return value we
    // capture) — same lib_header.json-driven mechanism as callStatement().
    ExprResult parseModuleCallExpr(const string& moduleName) {
        this->advance(); // module name
        this->expart_type("DOT", "Expected '.'");
        this->advance();
        this->expart_type("IDENTIFIER", "Expected function name after '" + moduleName + ".'");
        string funcName = this->current().value.get<string>();
        this->advance();

        if (find(this->import.begin(), this->import.end(), moduleName) == this->import.end())
            this->error("Module '" + moduleName + "' is not imported");
        if (!this->lib_header.contains(moduleName) || !this->lib_header[moduleName].contains("functions") ||
            !this->lib_header[moduleName]["functions"].contains(funcName))
            this->error("Unknown function '" + funcName + "' in module '" + moduleName + "'");

        json fn = this->lib_header[moduleName]["functions"][funcName];
        string symbol = fn["symbol"].get<string>();
        string retType = fn.value("returns", "i32");
        vector<string> paramTypes;
        bool typed = fn.contains("params") && fn["params"].is_array();
        if (typed) for (auto& p : fn["params"]) paramTypes.push_back(p.get<string>());

        this->expart_type("LPAREN", "Expected '(' after '" + moduleName + "." + funcName + "'");
        this->advance();
        vector<ExprResult> args;
        while (this->current().type != "RPAREN") {
            args.push_back(this->parseExpr());
            if (this->current().type == "COMMA") this->advance();
        }
        this->advance(); // ')'

        if (!typed) {
            // legacy single-string-argument path
            if (args.empty() || args[0].type != "ptr")
                this->error("'" + funcName + "' expects a string as its first argument");
            paramTypes.assign(args.size(), "ptr"); // best-effort; only arg 0 is actually used below
        }
        if (args.size() != paramTypes.size())
            this->error("'" + funcName + "' expects " + to_string(paramTypes.size()) +
                        " argument(s) but got " + to_string(args.size()));

        ostringstream call;
        string retReg;
        if (retType != "void") { retReg = this->writer.newTemp(); call << retReg << " = "; }
        call << "call " << retType << " @" << symbol << "(";
        for (size_t i = 0; i < args.size(); ++i) {
            ExprResult a = this->coerce(args[i], paramTypes[i], "argument " + to_string(i + 1) + " of " + funcName);
            if (i) call << ", ";
            call << paramTypes[i] << " " << a.value;
        }
        call << ")";
        this->writer.emit(call.str());
        return ExprResult{retType, retType == "void" ? "" : retReg};
    }

    // ── arithmetic / comparison / coercion ─────────────────────────────

    ExprResult widenForArith(ExprResult a, ExprResult b, ExprResult& outA, ExprResult& outB) {
        // returns the common llvm type; widens int->double as needed
        if (a.type == "double" || b.type == "double") {
            outA = this->coerce(a, "double", "left operand");
            outB = this->coerce(b, "double", "right operand");
            return ExprResult{"double", ""};
        }
        outA = a; outB = b;
        return ExprResult{a.type, ""};
    }

    ExprResult emitArith(const string& op, ExprResult a, ExprResult b) {
        if (a.type == "ptr" && b.type == "ptr" && op == "PLUS") {
            this->writer.addDeclare("declare ptr @tt_str_concat(ptr, ptr)");
            string r = this->writer.newTemp();
            this->writer.emit(r + " = call ptr @tt_str_concat(ptr " + a.value + ", ptr " + b.value + ")");
            return ExprResult{"ptr", r};
        }
        ExprResult ca, cb;
        ExprResult common = this->widenForArith(a, b, ca, cb);
        string ty = common.type;
        if (ty != "i32" && ty != "double")
            this->error("arithmetic is only supported between int/float values (and '+' between two strings)");

        string r = this->writer.newTemp();
        string instr;
        if (ty == "i32") {
            if (op == "PLUS") instr = "add";
            else if (op == "MINUS") instr = "sub";
            else if (op == "MUL") instr = "mul";
            else instr = "sdiv";
        } else {
            if (op == "PLUS") instr = "fadd";
            else if (op == "MINUS") instr = "fsub";
            else if (op == "MUL") instr = "fmul";
            else instr = "fdiv";
        }
        this->writer.emit(r + " = " + instr + " " + ty + " " + ca.value + ", " + cb.value);
        return ExprResult{ty, r};
    }

    ExprResult emitComparison(const string& op, ExprResult a, ExprResult b) {
        static const unordered_map<string, string> icmpOp = {
            {"EQ", "eq"}, {"NEQ", "ne"}, {"GT", "sgt"}, {"LT", "slt"}, {"GTE", "sge"}, {"LTE", "sle"}};
        static const unordered_map<string, string> fcmpOp = {
            {"EQ", "oeq"}, {"NEQ", "one"}, {"GT", "ogt"}, {"LT", "olt"}, {"GTE", "oge"}, {"LTE", "ole"}};

        if (a.type == "ptr" && b.type == "ptr") {
            if (op != "EQ" && op != "NEQ") this->error("only == and != are supported between strings");
            this->writer.addDeclare("declare i8 @tt_str_eq(ptr, ptr)");
            string r = this->writer.newTemp();
            this->writer.emit(r + " = call i8 @tt_str_eq(ptr " + a.value + ", ptr " + b.value + ")");
            string b8 = this->writer.newTemp();
            this->writer.emit(b8 + " = icmp " + string(op == "EQ" ? "ne" : "eq") + " i8 " + r + ", 0");
            return ExprResult{"i1", b8};
        }

        ExprResult ca, cb;
        ExprResult common = this->widenForArith(a, b, ca, cb);
        string ty = common.type;
        string r = this->writer.newTemp();
        if (ty == "i32" || ty == "i1" || ty == "i8" || ty == "i64") {
            this->writer.emit(r + " = icmp " + icmpOp.at(op) + " " + ty + " " + ca.value + ", " + cb.value);
        } else if (ty == "double") {
            this->writer.emit(r + " = fcmp " + fcmpOp.at(op) + " double " + ca.value + ", " + cb.value);
        } else {
            this->error("comparison is not supported for type '" + ty + "'");
        }
        return ExprResult{"i1", r};
    }

    // Boolean-ify a value for use as an `if`/`while` condition. i1 passes
    // through; i8/i32/i64 are truthy-tested against 0 (this also covers
    // built-ins like jsonhas()/streq() that return i8 rather than i1 to
    // avoid the C-ABI pitfalls of declaring an `i1` return type — see
    // README.md).
    ExprResult ensureBool(ExprResult v) {
        if (v.type == "i1") return v;
        if (v.type == "i8" || v.type == "i32" || v.type == "i64") {
            string r = this->writer.newTemp();
            this->writer.emit(r + " = icmp ne " + v.type + " " + v.value + ", 0");
            return ExprResult{"i1", r};
        }
        this->error("condition must be a bool (or int/char) expression, got '" + v.type + "'");
    }

    // Coerces `v` to `targetType`, widening int literals/values to double
    // automatically. Anything else that doesn't already match is a hard
    // type error (no implicit narrowing, no implicit ptr<->scalar casts).
    ExprResult coerce(ExprResult v, const string& targetType, const string& context) {
        if (v.type == targetType) return v;
        if (targetType == "double" && v.type == "i32") {
            string r = this->writer.newTemp();
            this->writer.emit(r + " = sitofp i32 " + v.value + " to double");
            return ExprResult{"double", r};
        }
        if (targetType == "i8" && v.type == "i1") {
            string r = this->writer.newTemp();
            this->writer.emit(r + " = zext i1 " + v.value + " to i8");
            return ExprResult{"i8", r};
        }
        if (targetType == "i64" && v.type == "i32") {
            string r = this->writer.newTemp();
            this->writer.emit(r + " = sext i32 " + v.value + " to i64");
            return ExprResult{"i64", r};
        }
        this->error("type mismatch for " + context + ": expected '" + targetType + "' but got '" + v.type + "'");
    }

    // ─────────────────────────────────────────────────────────────────
    //  ORIGINAL FFI SURFACE  (import "lib"; / module.function(...); as a
    //  statement) — unchanged in behaviour from v1, just re-plumbed to
    //  emit through ModuleWriter instead of owning its own vectors.
    // ─────────────────────────────────────────────────────────────────

    void parenStatement(vector<Token>& tokens) {
        this->expart_type("LPAREN", "Expected '(' after function name");
        this->advance();
        while (this->current().type != "RPAREN") {
            if (this->current().type == "SEMICOLON") this->error("SyntaxError: Expected ')'");
            tokens.push_back(this->current());
            this->advance();
        }
        this->advance();
        this->expectSemi();
    }

    vector<vector<Token>> splitArgs(vector<Token>& tokens) {
        vector<vector<Token>> args;
        vector<Token> cur;
        for (auto& t : tokens) {
            if (t.type == "COMMA") { args.push_back(cur); cur.clear(); }
            else cur.push_back(t);
        }
        if (!cur.empty()) args.push_back(cur);
        return args;
    }

    string lowerArgToLLVM(const string& paramType, vector<Token>& argTokens, const string& funcName) {
        if (argTokens.empty()) this->error("'" + funcName + "' called with a missing argument");
        if (argTokens.size() > 1)
            this->error("'" + funcName + "': arguments must currently be a single literal "
                        "(int/float/string) — expressions and variables aren't supported yet");
        Token& t = argTokens[0];

        if (paramType == "ptr") {
            if (t.type != "STRING") this->error("'" + funcName + "' expects a string literal for a 'ptr' argument");
            return "ptr " + this->internStringLiteral(t.value.get<string>());
        }
        if (paramType == "i1" || paramType == "i8" || paramType == "i16" || paramType == "i32" || paramType == "i64") {
            if (t.type != "INT") this->error("'" + funcName + "' expects an integer literal for a '" + paramType + "' argument");
            return paramType + " " + t.value.dump();
        }
        if (paramType == "float" || paramType == "double") {
            if (t.type != "FLOAT" && t.type != "INT")
                this->error("'" + funcName + "' expects a numeric literal for a '" + paramType + "' argument");
            ostringstream num;
            if (t.type == "FLOAT") num << std::showpoint << t.value.get<double>();
            else num << t.value.get<long long>() << ".0";
            return paramType + " " + num.str();
        }
        this->error("'" + funcName + "': unsupported param type '" + paramType +
                    "' declared in lib_header.json (expected ptr / i1 / i8 / i16 / i32 / i64 / float / double)");
    }

    void importStatement() {
        this->advance();
        this->expart_type("STRING", "import name is not STRING type");
        string name = this->current().value.get<string>();
        this->advance();
        this->expectSemi();

        if (!this->lib_header.contains(name)) this->error("Unknown library '" + name + "'");
        this->import.push_back(name);
        this->writer.importLibrary(name, this->lib_header);

        if (this->lib_header[name].contains("source")) {
            for (auto& s : this->lib_header[name]["source"]) {
                fs::path srcPath = s.get<string>();
                if (srcPath.is_relative()) srcPath = (this->ProjectPath / srcPath).lexically_normal();
                if (find(this->externSources.begin(), this->externSources.end(), srcPath) == this->externSources.end())
                    this->externSources.push_back(srcPath);
            }
        }
        if (this->lib_header[name].contains("libs")) {
            for (auto& l : this->lib_header[name]["libs"]) {
                string libName = l.get<string>();
                if (find(this->externLibs.begin(), this->externLibs.end(), libName) == this->externLibs.end())
                    this->externLibs.push_back(libName);
            }
        }
    }

    void includeStatement() {
        if (this->current().type == "HASH") this->advance();
        if (this->atKeyword("include")) this->advance();

        while (this->current().type != "NEWLINE" && this->current().type != "SEMICOLON" &&
               this->current().type != "EOF") {
            this->advance();
        }
        if (this->current().type == "NEWLINE") this->advance();
    }

    void linkStatement() {
        this->advance(); // 'link'
        this->expart_type("STRING", "link path is not STRING type");
        string linkPathText = this->current().value.get<string>();
        this->advance();
        this->expectSemi();

        fs::path linkPath = fs::path(linkPathText);
        if (linkPath.is_relative()) {
            // project-local `link` paths resolve relative to the active project
            // directory, while compiler-owned resources continue to use MotherPath.
            linkPath = (this->ProjectPath / linkPath).lexically_normal();
        } else {
            linkPath = linkPath.lexically_normal();
        }
        linkPath = fs::absolute(linkPath);

        if (!fs::exists(linkPath)) {
            this->error("Linked file not found: " + linkPath.string());
        }

        if (find(this->linkedFiles.begin(), this->linkedFiles.end(), linkPath) != this->linkedFiles.end()) {
            return; // already linked this file, ignore duplicate
        }
        this->linkedFiles.push_back(linkPath);

        ifstream linkedFile(linkPath);
        if (!linkedFile.is_open()) {
            this->error("Cannot open linked file: " + linkPath.string());
        }
        stringstream ss;
        ss << linkedFile.rdbuf();
        linkedFile.close();

        MONO_HPP::Lexer lexer(ss.str(), 4);
        vector<Token> linkedTokens = lexer.run();
        if (!linkedTokens.empty() && linkedTokens.back().type == "EOF") {
            linkedTokens.pop_back();
        }
        this->tokens.insert(this->tokens.begin() + this->pc, linkedTokens.begin(), linkedTokens.end());
    }

    void callStatement() {
        string moduleName = this->current().value.get<string>();
        int callLine = this->current().line, callCol = this->current().col;
        this->advance();
        if (this->current().type != "DOT" && this->current().type != "SCOPE") {
            this->error("Expected '.' or '::' after module name '" + moduleName + "'");
        }
        this->advance();

        static const vector<string> nonNameTypes = {
            "LPAREN", "RPAREN", "LBRACE", "RBRACE", "LBRACKET", "RBRACKET",
            "SEMICOLON", "COMMA", "COLON", "DOT", "EOF", "NEWLINE",
            "STRING", "INT", "FLOAT", "CHAR", "BOOL",
            "PLUS", "MINUS", "MUL", "DIV", "ASSIGN", "EQ", "NEQ", "GT", "LT", "GTE", "LTE"};
        if (!this->current().value.is_string() ||
            find(nonNameTypes.begin(), nonNameTypes.end(), this->current().type) != nonNameTypes.end())
            this->error("Expected function name after '" + moduleName + ".'");
        string funcName = this->current().value.get<string>();
        this->advance();

        if (find(this->import.begin(), this->import.end(), moduleName) == this->import.end())
            this->error("Module '" + moduleName + "' is not imported");
        if (!this->lib_header.contains(moduleName) || !this->lib_header[moduleName].contains("functions") ||
            !this->lib_header[moduleName]["functions"].contains(funcName))
            this->error("Unknown function '" + funcName + "' in module '" + moduleName + "'");

        vector<Token> rawArgs;
        this->parenStatement(rawArgs);
        vector<vector<Token>> args = this->splitArgs(rawArgs);

        json funcInfo = this->lib_header[moduleName]["functions"][funcName];
        string symbol = funcInfo["symbol"].get<string>();
        string retType = funcInfo.value("returns", "i32");

        if (funcInfo.contains("params") && funcInfo["params"].is_array()) {
            vector<string> paramTypes;
            for (auto& p : funcInfo["params"]) paramTypes.push_back(p.get<string>());

            bool argsEmptyCall = (args.size() == 1 && args[0].empty());
            size_t actualArgCount = (paramTypes.empty() && argsEmptyCall) ? 0 : args.size();
            if (actualArgCount != paramTypes.size())
                this->error("'" + funcName + "' expects " + to_string(paramTypes.size()) +
                            " argument(s) but got " + to_string(actualArgCount));

            vector<string> llvmArgs;
            for (size_t i = 0; i < paramTypes.size(); ++i)
                llvmArgs.push_back(this->lowerArgToLLVM(paramTypes[i], args[i], funcName));

            ostringstream call;
            call << "call " << retType << " @" << symbol << "(";
            for (size_t i = 0; i < llvmArgs.size(); ++i) { if (i) call << ", "; call << llvmArgs[i]; }
            call << ")";
            this->writer.emit(call.str());
            return;
        }

        if (args.empty() || args[0].empty() || args[0][0].type != "STRING")
            this->error("'" + funcName + "' expects a string as its first argument");
        string label = this->internStringLiteral(args[0][0].value.get<string>());
        this->writer.emit("call " + retType + " @" + symbol + "(ptr " + label + ")");

        if (args.size() > 1) {
            cout << "[Warning] " << funcName << "(): extra arguments ignored (only the string arg is used) at "
                 << callLine << ":" << callCol
                 << " -- add a \"params\" array to this function in lib_header.json to accept more/typed arguments" << endl;
        }
    }

    // ─────────────────────────────────────────────────────────────────
    //  project-local lib.json  (unchanged from v1)
    // ─────────────────────────────────────────────────────────────────
    void resolveLibSourcesInPlace(json& libSet, const fs::path& baseDir) {
        for (auto& item : libSet.items()) {
            json& libEntry = item.value();
            if (!libEntry.is_object() || !libEntry.contains("source")) continue;
            for (auto& s : libEntry["source"]) {
                if (!s.is_string()) continue;
                fs::path p = s.get<string>();
                if (p.is_relative()) s = (baseDir / p).lexically_normal().string();
            }
        }
    }

    void mergeProjectLibFile(const fs::path& dir) {
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
        } catch (const json::parse_error& e) {
            cout << "[Error] Failed to parse " << projectLibPath << ": " << e.what() << endl;
            exit(1);
        }
        this->resolveLibSourcesInPlace(projectLib, dir);
        this->lib_header.merge_patch(projectLib);
        cout << "[Info] Linked project lib file: " << projectLibPath << endl;
    }
};

#endif