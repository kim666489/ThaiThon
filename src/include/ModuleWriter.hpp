// ─────────────────────────────────────────────────────────────────────────
//  ModuleWriter.hpp
//
//  A small, front-end-agnostic LLVM-IR text emitter.
//
//  In the previous Parser.hpp, the class doing token parsing ALSO owned
//  llvmDeclares/llvmGlobals/llvmBody and the emitLLVM() assembly logic.
//  That's fine for one small parser, but it means every front-end (the
//  hand-rolled ThaiThon Parser, and now mono.hpp's JSON-grammar
//  Normalizer) would have to duplicate string-escaping, string interning,
//  declare de-duplication, etc.
//
//  ModuleWriter pulls all of that codegen bookkeeping into one reusable
//  class. A front-end's job becomes: figure out *what* to call with
//  *what* arguments, and hand that to a ModuleWriter instance. This file
//  has no dependency on Parser.hpp or mono.hpp — either (or both) can
//  #include it.
// ─────────────────────────────────────────────────────────────────────────
#ifndef MODULE_WRITER
#define MODULE_WRITER

#include "nlohmann/json.hpp"
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

class ModuleWriter {
public:
    explicit ModuleWriter(std::string moduleName = "thaithon_module")
        : moduleName_(std::move(moduleName)) {}

    // ── library / import management ─────────────────────────────────────

    // Registers `name` as imported and pulls in its "declare" lines from
    // libHeader (deduplicated, order-preserving). Returns false if `name`
    // isn't a key in libHeader at all (unknown library) — caller decides
    // whether that's a hard error.
    bool importLibrary(const std::string& name, const json& libHeader) {
        if (!libHeader.contains(name)) return false;
        imported_.insert(name); // idempotent

        if (libHeader[name].contains("declare")) {
            for (auto& d : libHeader[name]["declare"]) {
                addDeclare(d.get<std::string>());
            }
        }
        return true;
    }

    bool isImported(const std::string& name) const {
        return imported_.count(name) != 0;
    }

    // Adds a raw `declare ...` line if it isn't already present.
    void addDeclare(const std::string& declareLine) {
        if (declareSet_.insert(declareLine).second) {
            declares_.push_back(declareLine);
        }
    }

    // ── string constants ────────────────────────────────────────────────

    // Interns a string literal as a global constant, returning the LLVM
    // pointer expression (e.g. "@.str.0") to reference it. Calling this
    // twice with the same text reuses the existing global instead of
    // emitting a duplicate constant.
    std::string internString(const std::string& text) {
        auto it = stringPool_.find(text);
        if (it != stringPool_.end()) return it->second;

        std::string label = "@.str." + std::to_string(stringCounter_++);
        int len = static_cast<int>(text.size()) + 1; // +1 for the NUL terminator
        globals_.push_back(label + " = private unnamed_addr constant [" +
                            std::to_string(len) + " x i8] c\"" + escape(text) + "\\00\"");
        stringPool_[text] = label;
        return label;
    }

    // ── calls / instructions ────────────────────────────────────────────

    // Emits `call <retType> @<symbol>(<llvmArgs...>)` into @main's body.
    // `llvmArgs` entries must already be fully-formed LLVM operand text,
    // e.g. "ptr @.str.0" or "i32 3".
    void emitCall(const std::string& symbol, const std::string& retType,
                  const std::vector<std::string>& llvmArgs) {
        std::ostringstream call;
        call << "call " << retType << " @" << symbol << "(";
        for (size_t i = 0; i < llvmArgs.size(); ++i) {
            if (i) call << ", ";
            call << llvmArgs[i];
        }
        call << ")";
        body_.push_back(call.str());
    }

    // Convenience path for the common `module.function("literal")` shape
    // (e.g. stdio.print("Hello World")). Looks `funcName` up under
    // libHeader[moduleName]["functions"], interns `stringArg`, and emits
    // the call. Returns false (emitting nothing) if the module isn't
    // imported or the function isn't declared in libHeader.
    //
    // If the function's declared "params" has more than one entry,
    // *warningOut (when non-null) is set to a message the caller can
    // surface — this path only ever emits the single string argument.
    bool emitStringCall(const std::string& moduleName, const std::string& funcName,
                         const std::string& stringArg, const json& libHeader,
                         std::string* warningOut = nullptr) {
        if (!isImported(moduleName)) return false;
        if (!libHeader.contains(moduleName) ||
            !libHeader[moduleName].contains("functions") ||
            !libHeader[moduleName]["functions"].contains(funcName)) {
            return false;
        }

        json fn = libHeader[moduleName]["functions"][funcName];
        std::string symbol  = fn.value("symbol", funcName);
        std::string retType = fn.value("returns", "i32");
        std::vector<std::string> params =
            fn.value("params", std::vector<std::string>{"ptr"});

        std::string ptrLabel = internString(stringArg);
        emitCall(symbol, retType, {"ptr " + ptrLabel});

        if (params.size() > 1 && warningOut) {
            *warningOut = funcName + "(): only the string argument was emitted; " +
                          std::to_string(params.size() - 1) +
                          " extra declared parameter(s) aren't handled by "
                          "emitStringCall() yet";
        }
        return true;
    }

    // Escape hatch: drop an already-formatted LLVM instruction straight
    // into @main's body, for anything the convenience methods above don't
    // cover yet (arithmetic, branches, etc.).
    void emitRaw(const std::string& instruction) {
        body_.push_back(instruction);
    }

    // ── output ───────────────────────────────────────────────────────────

    std::string toString() const {
        std::ostringstream out;
        out << "; ModuleID = '" << moduleName_ << "'\n\n";
        for (auto& d : declares_) out << d << "\n";
        if (!declares_.empty()) out << "\n";
        for (auto& g : globals_) out << g << "\n";
        if (!globals_.empty()) out << "\n";
        out << "define i32 @main() {\n";
        out << "entry:\n";
        for (auto& b : body_) out << "  " << b << "\n";
        out << "  ret i32 0\n";
        out << "}\n";
        return out.str();
    }

    void write(const fs::path& outputPath) const {
        std::ofstream out(outputPath);
        if (!out.is_open()) {
            throw std::runtime_error("ModuleWriter::write: cannot open " + outputPath.string());
        }
        out << toString();
    }

private:
    static std::string escape(const std::string& text) {
        std::string escaped;
        escaped.reserve(text.size());
        for (char c : text) {
            if (c == '"' || c == '\\') escaped += '\\';
            escaped += c;
        }
        return escaped;
    }

    std::string moduleName_;
    std::set<std::string> imported_;
    std::vector<std::string> declares_;
    std::set<std::string> declareSet_;
    std::vector<std::string> globals_;
    std::unordered_map<std::string, std::string> stringPool_;
    int stringCounter_ = 0;
    std::vector<std::string> body_;
};

#endif // MODULE_WRITER