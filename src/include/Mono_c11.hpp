// ─────────────────────────────────────────────────────────────────────────
//  mono.hpp  —  single-header C++ port of Mono_py11.py
//                (lexer + JSON-grammar normalizer)
//
//  Merged single-file version: this used to be split into mono.hpp
//  (declarations) + mono.cpp (definitions + main()). Everything now lives
//  in this one header so you can drop it into a project with nothing else
//  to add or build separately.
//
//  All free functions, out-of-class member functions, and namespace-scope
//  variables below are marked `inline` (C++17 inline variables/functions)
//  so this header is safe to #include from more than one translation unit
//  without violating the One Definition Rule.
//
//  Two real bugs found in the original Python source were fixed here (and
//  should be fixed there too):
//
//   BUG 1 (rule backtracking): in the "token type match" branch of the rule
//   walker, when every alternative for a matched token fails, the original
//   code did `self.pos -= 1` which assumes pos is still exactly one past the
//   consumed token. But a failed nested alternative may have advanced pos
//   further without rolling back, so this could over- or under-backtrack.
//   Fixed here by restoring to `saved_pos - 1` (the position captured right
//   after the single consume, minus that one consume) instead of a blind -1
//   off whatever pos happens to be.
//
//   BUG 2 (_eval_token_seq hardcoded bracket types): the function received
//   open_type/close_type parameters but compared literal "LBRACKET"/"LBRACE"
//   strings instead of the parameters, so custom bracket token names would
//   never be recognised as nested list/map. Fixed to compare against the
//   actual parameters.
//
//   BUG 3 (infinite loop on "warn" policy): if a rule matched but produced
//   no result and the "rule_not_matched" error policy is "warn" (so no
//   exception is thrown), the original logic could re-enter the loop
//   pointed at the exact same token forever. Fixed by forcing forward
//   progress (consuming the token) when position didn't move.
//
//  Build:  g++ -std=c++17 -O2 -x c++ mono.hpp -o mono
//      or: cp mono.hpp mono.cpp && g++ -std=c++17 -O2 mono.cpp -o mono
//  Run:    ./mono <source_file> <rule.json>
//          ./mono --init <rule_name>
// ─────────────────────────────────────────────────────────────────────────
#ifndef MONO_HPP
#define MONO_HPP

#include "nlohmann/json.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;   // ordered_json: preserves key insertion
                                        // order, matching Python dict semantics

// ═════════════════════════════════════════════════════════════════════════
//  DEFAULT / TEMPLATE GRAMMARS  (defined in mono.cpp)
// ═════════════════════════════════════════════════════════════════════════

extern const json DEFAULT_GRAMMAR;      // embedded fallback grammar
extern const json RULE_DATA_TEMPLATE;   // template written out by --init

// ═════════════════════════════════════════════════════════════════════════
//  RUNTIME STATE  (loaded from rule.json; defined in mono.cpp)
// ═════════════════════════════════════════════════════════════════════════

struct BracketSpec {
    std::string open_type;
    std::string close_type;
};

extern std::unordered_map<std::string, std::optional<std::set<std::string>>> COLLECT_TYPES;
extern std::unordered_map<std::string, std::optional<BracketSpec>>           COLLECT_BRACKETS;
extern std::unordered_map<std::string, std::unordered_map<std::string, std::string>> COLLECT_DELIMITERS;
extern std::set<std::string> COLLECT_EXPR_MODE;
extern json                  CUSTOM_STRING_MODES;
extern std::unordered_map<std::string, std::string> ERROR_POLICY;
extern bool                  ENABLE_CHAR_TYPE;
extern std::string           grammar_path;
extern json                  lexer_rule;   // {keyword, operator, symbol}

extern const std::unordered_map<std::string, std::string> DEFAULT_ERROR_POLICY;
extern const std::unordered_map<std::string, std::unordered_map<std::string, std::string>> BUILTIN_COLLECT_DELIMITERS;

// ── IR type registry ───────────────────────────────────────────────────
extern const std::unordered_map<std::string, std::string> ACTION_TYPE_MAP;
extern const std::unordered_map<std::string, std::string> COLLECT_DATA_TYPE_MAP;

// ═════════════════════════════════════════════════════════════════════════
//  ERROR HANDLING
// ═════════════════════════════════════════════════════════════════════════

// Looks up error_type in `policy` (or ERROR_POLICY/DEFAULT_ERROR_POLICY when
// policy is null); throws std::runtime_error on "exit", otherwise logs a
// warning to stderr.
void handle_error(const std::string& error_type, const std::string& message,
                   const std::unordered_map<std::string, std::string>* policy = nullptr);

// ═════════════════════════════════════════════════════════════════════════
//  IR TYPE RESOLUTION
// ═════════════════════════════════════════════════════════════════════════

std::string resolve_ir_type(const std::string& action);
json resolve_data_type_detailed(const std::optional<std::string>& collect_mode, const json& data);

// ═════════════════════════════════════════════════════════════════════════
//  BRACKET / SCHEMA / RULE-FILE HELPERS
// ═════════════════════════════════════════════════════════════════════════

std::optional<BracketSpec> parse_bracket_entry(const json& entry);
void validate_schema(const json& rule_data, const std::unordered_map<std::string, std::string>& policy);

// Loads rule.json (or throws if it can't be opened) and populates the
// runtime state above (COLLECT_TYPES, COLLECT_BRACKETS, ERROR_POLICY, ...).
void load_rule(const std::string& path = "./rule.json");

// Writes RULE_DATA_TEMPLATE to "./<rule_name>.json" (used by `mono --init`).
void create_rule_template(const std::string& rule_name);

// ═════════════════════════════════════════════════════════════════════════
//  TOKEN
// ═════════════════════════════════════════════════════════════════════════

struct Token {
    std::string type;
    json        value;      // string / int64 / double payload as parsed by the lexer
    size_t      pos  = 0;
    int         line = 0;
    int         col  = 0;
    int         tab  = 0;

    std::string repr() const;
};

// ═════════════════════════════════════════════════════════════════════════
//  LEXER — Maximal Munch
// ═════════════════════════════════════════════════════════════════════════

class Lexer {
public:
    explicit Lexer(std::string code, int tab_size = 4);

    std::vector<Token> run();

private:
    enum class State {
        Start, Identifier, Number, StringLit, CharLit,
        CommentLine, CommentBlock, CustomStringBody, Done
    };

    std::string code_;
    size_t pos_ = 0;
    int line_ = 1, col_ = 0, tab_size_;
    std::optional<char> current_char_;
    std::string buffer_;
    std::vector<Token> tokens_;
    int line_tab_ = 0;
    State state_ = State::Start;

    json keywords_ = json::object(), operators_ = json::object(), symbols_ = json::object();
    std::unordered_map<std::string, json> custom_strings_;
    std::vector<std::tuple<std::string, std::string, std::string>> munch_table_;
    json active_custom_cfg_;

    void calc_line_tab();
    void advance();
    std::optional<char> peek(int offset = 1) const;
    std::string peek_str(size_t length) const;
    void emit(const std::string& type, const json& value);
    std::optional<std::tuple<std::string, std::string, std::string>> try_munch() const;
    void consume_chars(size_t n);
    void step();

    void state_start();
    void state_custom_string_body();
    void state_identifier();
    void state_number();
    void state_string();
    void state_char();
    void state_comment_line();
    void state_comment_block();
};

// ═════════════════════════════════════════════════════════════════════════
//  TOKEN TYPE / VALUE HELPERS
// ═════════════════════════════════════════════════════════════════════════

std::string detect_token_type(const Token& token);
json coerce_value(const Token& token);
json coerce_token(const Token& token, bool include_type = true);

// ═════════════════════════════════════════════════════════════════════════
//  DELIMITER HELPERS
// ═════════════════════════════════════════════════════════════════════════

std::string get_item_sep(const std::string& cmd_name);
std::string get_kv_sep(const std::string& cmd_name);

// ═════════════════════════════════════════════════════════════════════════
//  LIST / MAP / TOKEN-SEQUENCE EVALUATION (mutually recursive)
// ═════════════════════════════════════════════════════════════════════════

json eval_token_seq(const std::vector<Token>& tokens, const std::string& open_type,
                     const std::string& close_type, const std::string& item_sep = "COMMA");

json parse_list_tokens(const std::vector<Token>& raw_tokens, const std::string& open_type,
                        const std::string& close_type, const std::string& item_sep = "COMMA");

json parse_map_tokens(const std::vector<Token>& raw_tokens, const std::string& open_type = "LBRACE",
                       const std::string& close_type = "RBRACE", const std::string& item_sep = "COMMA",
                       const std::string& kv_sep = "COLON");

// ═════════════════════════════════════════════════════════════════════════
//  NORMALIZER  (parser: walks the JSON grammar rule tree against the tokens)
// ═════════════════════════════════════════════════════════════════════════

class Normalizer {
public:
    Normalizer(std::vector<Token> tokens, const json& grammar);

    std::vector<json> parse();

private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;
    std::vector<json> ir_;
    std::string line_terminator_;
    std::optional<std::string> stmt_separator_;
    std::optional<std::string> sep_token_type_;
    json rules_;
    std::optional<int> current_tab_;
    std::unordered_map<std::string, std::string> error_policy_;
    std::optional<std::string> last_collect_mode_;

    static std::optional<std::string> resolve_sep_type(const std::optional<std::string>& sep_char);

    void err(const std::string& type, const std::string& message);
    Token* peek(int k = 0);
    Token consume();
    static std::string loc(const Token* tok);
    bool is_terminator(const Token* tok);
    bool is_separator(const Token* tok);
    static std::string resolve_bracket_token_type(const json& spec);

    json collect_until_close(const json& open_spec, const json& close_spec,
                              const std::optional<std::set<std::string>>& allowed,
                              const std::string& cmd_name);
    json collect_code_block();
    json collect_expr(const std::string& cmd_name);
    json collect_simple(const std::string& cmd_name);
    json collect(const std::string& cmd_name, int tab);

    std::optional<std::pair<std::string, std::vector<json>>>
    walk_rule(const json& rule_node, int tab, std::vector<json> accumulated);
};

// ═════════════════════════════════════════════════════════════════════════
//  IR SUMMARY HELPERS
// ═════════════════════════════════════════════════════════════════════════

std::string get_data_type_string(const json& ir_node);
void print_ir_summary(const std::vector<json>& ir_list);

// ═════════════════════════════════════════════════════════════════════════
//  IMPLEMENTATION
//  (declarations above provide every forward reference this section needs,
//   e.g. eval_token_seq is declared before parse_list_tokens/parse_map_tokens
//   use it, and the Token/Lexer/Normalizer classes are fully declared above
//   before their out-of-class inline method bodies below.)
// ═════════════════════════════════════════════════════════════════════════

inline const json DEFAULT_GRAMMAR = json::parse(R"JSON(
{
  "_config": {
    "line_terminator": ";",
    "stmt_separator": ",",
    "tab_size": 4,
    "keep_newline": false,
    "enable_char_type": true
  },
  "rules": {
    "INT":    {"collect": {"ASSIGN": {"collectint":    {"end": [";", "LetInt_Cmd"]}}}},
    "STRING": {"collect": {"ASSIGN": {"collectstring": {"end": [";", "LetString_Cmd"]}}}},
    "FLOAT":  {"collect": {"ASSIGN": {"collectfloat":  {"end": [";", "LetFloat_Cmd"]}}}},
    "DOUBLE": {"collect": {"ASSIGN": {"collectdouble": {"end": [";", "LetDouble_Cmd"]}}}},
    "CHAR":   {"collect": {"ASSIGN": {"collectchar":   {"end": [";", "LetChar_Cmd"]}}}},
    "BOOL":   {"collect": {"ASSIGN": {"collectbool":   {"end": [";", "LetBool_Cmd"]}}}},
    "AUTO":   {"collect": {"ASSIGN": {"collectauto":   {"end": [";", "LetAuto_Cmd"]}}}},
    "IDENTIFIER": {"ASSIGN": {"collectexpr": {"end": [";", "Assign_Cmd"]}}}
  }
}
)JSON");

inline const json RULE_DATA_TEMPLATE = json::parse(R"JSON(
{
  "_config": {
    "line_terminator": ";",
    "stmt_separator": "/",
    "tab_size": 4,
    "keep_newline": false,
    "enable_char_type": true
  },
  "_error_policy": {
    "unknown_token": "exit", "type_error": "exit", "unclosed_bracket": "exit",
    "unclosed_string": "exit", "invalid_char": "exit", "unknown_operator": "warn",
    "rule_not_matched": "warn", "schema_violation": "exit"
  },
  "rules": {},
  "lexer": {
    "keyword": {},
    "operator": {"+":"PLUS","-":"MINUS","*":"MUL","/":"DIV","=":"ASSIGN",
                 "==":"EQ","!=":"NEQ",">":"GT","<":"LT",">=":"GTE","<=":"LTE"},
    "symbol": {"(":"LPAREN",")":"RPAREN","{":"LBRACE","}":"RBRACE",
               "[":"LBRACKET","]":"RBRACKET",";":"SEMICOLON",",":"COMMA",":":"COLON"}
  },
  "normalizer": {
    "collect_types": {
      "collectint": ["INT"],
      "collectfloat": ["FLOAT","INT"],
      "collectdouble": ["FLOAT","INT"],
      "collectstring": ["STRING"],
      "collectchar": ["CHAR"],
      "collectbool": ["BOOL"],
      "collectauto": ["INT","FLOAT","STRING","CHAR","BOOL","IDENTIFIER","LBRACKET","RBRACKET","LBRACE","RBRACE","COMMA","COLON"],
      "collectexpr": ["INT","FLOAT","STRING","CHAR","BOOL","IDENTIFIER","PLUS","MINUS","MUL","DIV","LPAREN","RPAREN","EQ","NEQ","GT","LT","GTE","LTE"],
      "collectlist": ["INT","FLOAT","STRING","CHAR","BOOL","IDENTIFIER","COMMA","LBRACKET","RBRACKET"],
      "collectmap": ["INT","FLOAT","STRING","CHAR","BOOL","IDENTIFIER","COMMA","COLON","LBRACE","RBRACE","LBRACKET","RBRACKET"],
      "collectcode": null,
      "collect": null,
      "collectfunc": ["INT","FLOAT","STRING","CHAR","BOOL","IDENTIFIER","COMMA","LPAREN","RPAREN"]
    },
    "collect_brackets": {
      "collectlist": ["LBRACKET","RBRACKET"],
      "collectfunc": ["LPAREN","RPAREN"],
      "collectmap": ["LBRACE","RBRACE"],
      "collectcode": ["LBRACE","RBRACE"],
      "collectexpr": null
    },
    "collect_delimiters": {
      "collectlist": {"item_sep":"COMMA"},
      "collectmap": {"item_sep":"COMMA","kv_sep":"COLON"},
      "collectfunc": {"item_sep":"COMMA"}
    },
    "collect_string_modes": {},
    "collect_expr_mode": ["collectint","collectfloat","collectdouble","collectstring",
                           "collectchar","collectbool","collectauto","collectexpr"]
  }
}
)JSON");

inline std::unordered_map<std::string, std::optional<std::set<std::string>>> COLLECT_TYPES;
inline std::unordered_map<std::string, std::optional<BracketSpec>>           COLLECT_BRACKETS;
inline std::unordered_map<std::string, std::unordered_map<std::string, std::string>> COLLECT_DELIMITERS;
inline std::set<std::string> COLLECT_EXPR_MODE;
inline json                   CUSTOM_STRING_MODES = json::object();
inline std::unordered_map<std::string, std::string> ERROR_POLICY;
inline bool                   ENABLE_CHAR_TYPE = true;
inline std::string            grammar_path = "./rule.json";
inline json                   lexer_rule = nullptr;

inline const std::unordered_map<std::string, std::string> DEFAULT_ERROR_POLICY = {
    {"unknown_token", "exit"}, {"type_error", "exit"}, {"unclosed_bracket", "exit"},
    {"unclosed_string", "exit"}, {"invalid_char", "exit"}, {"unknown_operator", "warn"},
    {"rule_not_matched", "warn"}, {"schema_violation", "exit"},
};

inline const std::unordered_map<std::string, std::unordered_map<std::string, std::string>> BUILTIN_COLLECT_DELIMITERS = {
    {"collectlist", {{"item_sep", "COMMA"}}},
    {"collectmap",  {{"item_sep", "COMMA"}, {"kv_sep", "COLON"}}},
    {"collectfunc", {{"item_sep", "COMMA"}}},
};

inline const std::unordered_map<std::string, std::string> ACTION_TYPE_MAP = {
    {"LetInt_Cmd", "let"}, {"LetString_Cmd", "let"}, {"LetFloat_Cmd", "let"},
    {"LetDouble_Cmd", "let"}, {"LetChar_Cmd", "let"}, {"LetBool_Cmd", "let"},
    {"LetAuto_Cmd", "let"}, {"Assign_Cmd", "assign"}, {"Call_Cmd", "call"},
    {"Return_Cmd", "return"}, {"If_Cmd", "if"}, {"While_Cmd", "while"},
    {"while_loop", "while"}, {"For_Cmd", "for"}, {"for_loop", "for"}, {"Block_Cmd", "block"},
};

inline const std::unordered_map<std::string, std::string> COLLECT_DATA_TYPE_MAP = {
    {"collectint", "int"}, {"collectfloat", "float"}, {"collectdouble", "double"},
    {"collectstring", "string"}, {"collectchar", "char"}, {"collectbool", "bool"},
    {"collectauto", "auto"}, {"collectexpr", "expr"}, {"collectlist", "list"},
    {"collectmap", "map"}, {"collectcode", "block"}, {"collectfunc", "func_args"},
    {"collect", "raw"},
};

inline void handle_error(const std::string& error_type, const std::string& message,
                   const std::unordered_map<std::string, std::string>* policy) {
    const auto& pol = policy ? *policy : (!ERROR_POLICY.empty() ? ERROR_POLICY : DEFAULT_ERROR_POLICY);
    std::string action = "exit";
    auto it = pol.find(error_type);
    if (it != pol.end()) action = it->second;
    if (action == "exit") {
        throw std::runtime_error(message);
    } else {
        std::cerr << "[Warning] " << message << "\n";
    }
}

inline std::string resolve_ir_type(const std::string& action) {
    auto it = ACTION_TYPE_MAP.find(action);
    if (it != ACTION_TYPE_MAP.end()) return it->second;

    std::string name = action;
    static const std::string suffix = "_Cmd";
    if (name.size() >= suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        name = name.substr(0, name.size() - suffix.size());
    }
    std::string snake;
    for (size_t i = 0; i < name.size(); ++i) {
        if (i > 0 && std::isupper(static_cast<unsigned char>(name[i]))) snake += '_';
        snake += static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
    }
    return snake;
}

inline json resolve_data_type_detailed(const std::optional<std::string>& collect_mode, const json& data) {
    std::string type_map = "unknown";
    if (collect_mode) {
        auto it = COLLECT_DATA_TYPE_MAP.find(*collect_mode);
        if (it != COLLECT_DATA_TYPE_MAP.end()) type_map = it->second;
    }

    std::string value_type = "unknown";
    auto classify_scalar = [](const json& v) -> std::string {
        if (v.is_boolean()) return "bool";
        if (v.is_number_integer()) return "int";
        if (v.is_number_float()) return "float";
        if (v.is_string()) {
            const std::string& s = v.get_ref<const std::string&>();
            if (s.size() == 1 && ENABLE_CHAR_TYPE) return "char";
            return "string";
        }
        return "unknown";
    };

    if (data.is_array() && !data.empty()) {
        const json& first_item = data[0];
        if (first_item.is_object() && first_item.contains("value")) {
            value_type = classify_scalar(first_item["value"]);
        } else if (first_item.is_object()) {
            value_type = "object";
        } else if (first_item.is_array()) {
            value_type = "list";
        } else {
            value_type = classify_scalar(first_item);
        }
    } else if (data.is_object()) {
        value_type = "map";
    } else if (!data.is_null()) {
        value_type = classify_scalar(data);
    }

    json out;
    out["type"] = type_map;
    out["value_type"] = value_type;
    out["collect_mode"] = collect_mode ? json(*collect_mode) : json(nullptr);
    return out;
}

inline std::optional<BracketSpec> parse_bracket_entry(const json& entry) {
    if (entry.is_null()) return std::nullopt;
    if (entry.is_array()) {
        if (entry.size() != 2)
            throw std::invalid_argument("collect_brackets array must have exactly 2 elements");
        return BracketSpec{ entry[0].get<std::string>(), entry[1].get<std::string>() };
    }
    if (entry.is_object()) {
        bool same = entry.value("same", false);
        std::string open_spec = entry.value("open", "");
        std::string close_spec = same ? open_spec : entry.value("close", "");
        return BracketSpec{ open_spec, close_spec };
    }
    throw std::invalid_argument("Invalid collect_brackets entry");
}

inline void validate_schema(const json& rule_data, const std::unordered_map<std::string, std::string>& policy) {
    using namespace std::string_literals;
    std::vector<std::string> errors;
    for (const std::string& key : {"_config"s, "rules"s, "lexer"s, "normalizer"s}) {
        if (!rule_data.contains(key)) errors.push_back("Missing required key: '" + key + "'");
    }
    json norm = rule_data.value("normalizer", json::object());
    for (const std::string& sub : {"collect_types"s, "collect_brackets"s, "collect_expr_mode"s}) {
        if (!norm.contains(sub)) errors.push_back("normalizer missing key: '" + sub + "'");
    }
    if (!errors.empty()) {
        std::string msg = "[SchemaError] rule.json has schema violations:\n";
        for (auto& e : errors) msg += "  \xe2\x80\xa2 " + e + "\n";
        handle_error("schema_violation", msg, &policy);
    }
}

inline void load_rule(const std::string& path) {
    std::ifstream f(path);
    json rule_data;
    if (f) {
        f >> rule_data;
    } else {
        rule_data = DEFAULT_GRAMMAR;
        throw std::runtime_error("[Error] could not open rule file: " + path);
    }

    json cfg = rule_data.value("_config", json::object());
    ENABLE_CHAR_TYPE = cfg.value("enable_char_type", true);

    json raw_policy = rule_data.value("_error_policy", json::object());
    ERROR_POLICY = DEFAULT_ERROR_POLICY;
    for (auto& [k, v] : raw_policy.items()) ERROR_POLICY[k] = v.get<std::string>();
    validate_schema(rule_data, ERROR_POLICY);

    lexer_rule = rule_data.at("lexer");
    json norm = rule_data.at("normalizer");
    grammar_path = path;

    COLLECT_TYPES.clear();
    json raw_ct = norm.value("collect_types", json::object());
    for (auto& [mode, types] : raw_ct.items()) {
        if (types.is_null()) { COLLECT_TYPES[mode] = std::nullopt; continue; }
        std::set<std::string> s;
        for (auto& t : types) s.insert(t.get<std::string>());
        COLLECT_TYPES[mode] = s;
    }

    COLLECT_BRACKETS.clear();
    json raw_cb = norm.value("collect_brackets", json::object());
    for (auto& [mode, entry] : raw_cb.items()) {
        COLLECT_BRACKETS[mode] = parse_bracket_entry(entry);
    }

    COLLECT_DELIMITERS.clear();
    for (auto& [mode, m] : BUILTIN_COLLECT_DELIMITERS) COLLECT_DELIMITERS[mode] = m;
    json raw_cd = norm.value("collect_delimiters", json::object());
    for (auto& [mode, conf] : raw_cd.items()) {
        auto& target = COLLECT_DELIMITERS[mode];
        for (auto& [k, v] : conf.items()) target[k] = v.get<std::string>();
    }

    CUSTOM_STRING_MODES = norm.value("collect_string_modes", json::object());

    COLLECT_EXPR_MODE.clear();
    json expr_list = norm.value("collect_expr_mode", json::array());
    if (expr_list.is_array())
        for (auto& e : expr_list) COLLECT_EXPR_MODE.insert(e.get<std::string>());
}

inline void create_rule_template(const std::string& rule_name) {
    std::ofstream f("./" + rule_name + ".json");
    f << RULE_DATA_TEMPLATE.dump(4);
    std::cout << "[Info] Created " << rule_name << ".json\n";
    std::cout << "[Warning] If you have a JSON file with the same name, please be careful not to overwrite the data.\n";
}

inline std::string Token::repr() const {
    std::ostringstream os;
    os << type << ":" << value.dump() << "(L" << line << ",C" << col << ",tab=" << tab << ")";
    return os.str();
}

inline Lexer::Lexer(std::string code, int tab_size)
    : code_(std::move(code)), tab_size_(tab_size) {
    if (!code_.empty()) current_char_ = code_[0];
    if (!lexer_rule.is_null()) {
        keywords_  = lexer_rule.value("keyword", json::object());
        operators_ = lexer_rule.value("operator", json::object());
        symbols_   = lexer_rule.value("symbol", json::object());
    }

    for (auto& [mode, cfg] : CUSTOM_STRING_MODES.items()) {
        std::string open_ch = cfg.value("open", "");
        if (!open_ch.empty()) {
            json c = cfg;
            c["_mode"] = mode;
            custom_strings_[open_ch] = c;
        }
    }

    for (auto& [op, tt] : operators_.items())  munch_table_.emplace_back(op, tt.get<std::string>(), "op");
    for (auto& [sy, tt] : symbols_.items())     munch_table_.emplace_back(sy, tt.get<std::string>(), "sym");
    for (auto& [opener, cfg] : custom_strings_)
        munch_table_.emplace_back(opener, cfg.value("token", "STRING"), "custom");

    std::sort(munch_table_.begin(), munch_table_.end(),
              [](const auto& a, const auto& b) {
                  if (std::get<0>(a).size() != std::get<0>(b).size())
                      return std::get<0>(a).size() > std::get<0>(b).size();
                  return std::get<0>(a) < std::get<0>(b);
              });

    calc_line_tab();
    state_ = State::Start;
}

inline std::vector<Token> Lexer::run() {
    while (state_ != State::Done) step();
    emit("EOF", json(nullptr));
    return tokens_;
}

inline void Lexer::calc_line_tab() {
    size_t p = pos_; int spaces = 0;
    while (p < code_.size() && (code_[p] == ' ' || code_[p] == '\t')) {
        spaces += (code_[p] == '\t') ? tab_size_ : 1;
        ++p;
    }
    line_tab_ = spaces / tab_size_;
}

inline void Lexer::advance() {
    if (current_char_ && *current_char_ == '\n') { ++line_; col_ = 0; }
    else { ++col_; }
    ++pos_;
    if (pos_ < code_.size()) {
        current_char_ = code_[pos_];
        if (col_ == 0) calc_line_tab();
    } else {
        current_char_.reset();
    }
}

inline std::optional<char> Lexer::peek(int offset) const {
    size_t nxt = pos_ + offset;
    if (nxt < code_.size()) return code_[nxt];
    return std::nullopt;
}

inline std::string Lexer::peek_str(size_t length) const {
    if (pos_ >= code_.size()) return "";
    return code_.substr(pos_, std::min(length, code_.size() - pos_));
}

inline void Lexer::emit(const std::string& type, const json& value) {
    tokens_.push_back(Token{type, value, pos_, line_, col_, line_tab_});
}

inline std::optional<std::tuple<std::string, std::string, std::string>> Lexer::try_munch() const {
    for (auto& [text, tok_type, kind] : munch_table_) {
        if (peek_str(text.size()) == text) return std::make_tuple(text, tok_type, kind);
    }
    return std::nullopt;
}

inline void Lexer::consume_chars(size_t n) { for (size_t i = 0; i < n; ++i) advance(); }

inline void Lexer::step() {
    switch (state_) {
        case State::Start:            state_start(); break;
        case State::Identifier:       state_identifier(); break;
        case State::Number:           state_number(); break;
        case State::StringLit:        state_string(); break;
        case State::CharLit:          state_char(); break;
        case State::CommentLine:      state_comment_line(); break;
        case State::CommentBlock:     state_comment_block(); break;
        case State::CustomStringBody: state_custom_string_body(); break;
        case State::Done: break;
    }
}

inline void Lexer::state_start() {
    if (!current_char_) { state_ = State::Done; return; }
    char c = *current_char_;

    if (c == '\n') { emit("NEWLINE", json("\\n")); advance(); return; }
    if (c == ' ' || c == '\t' || c == '\r') { advance(); return; }

    buffer_.clear();

    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') { state_ = State::Identifier; return; }
    if (std::isdigit(static_cast<unsigned char>(c))) { state_ = State::Number; return; }
    if (c == '"') { advance(); state_ = State::StringLit; return; }
    if (c == '\'') { advance(); state_ = State::CharLit; return; }

    auto match = try_munch();
    if (match) {
        auto [text, tok_type, kind] = *match;
        if (kind == "custom") {
            active_custom_cfg_ = custom_strings_[text];
            consume_chars(text.size());
            state_ = State::CustomStringBody;
            return;
        }
        if (kind == "op") {
            if (text == "//") { consume_chars(2); state_ = State::CommentLine; return; }
            if (text == "/*") { consume_chars(2); state_ = State::CommentBlock; return; }
        }
        emit(tok_type, json(text));
        consume_chars(text.size());
        return;
    }

    std::ostringstream msg;
    msg << "Unknown char at line " << line_ << ", col " << col_ << ": '" << c << "'";
    handle_error("unknown_token", msg.str());
    advance();
}

inline void Lexer::state_custom_string_body() {
    std::string close_ch = active_custom_cfg_.value("close", active_custom_cfg_.value("open", ""));
    std::string token_t  = active_custom_cfg_.value("token", "STRING");
    json escape = active_custom_cfg_.value("escape", json::object());

    while (true) {
        if (!current_char_) {
            std::ostringstream msg;
            msg << "Unclosed custom string (opener=" << active_custom_cfg_.value("open", "") << ") at line " << line_;
            handle_error("unclosed_string", msg.str());
            state_ = State::Done;
            return;
        }
        if (peek_str(close_ch.size()) == close_ch) {
            consume_chars(close_ch.size());
            emit(token_t, json(buffer_));
            state_ = State::Start;
            return;
        }
        if (!escape.is_null() && !escape.empty() && *current_char_ == '\\') {
            advance();
            std::string key(1, current_char_ ? *current_char_ : '\0');
            buffer_ += escape.value(key, key);
            advance();
            continue;
        }
        buffer_ += *current_char_;
        advance();
    }
}

inline void Lexer::state_identifier() {
    while (current_char_ && (std::isalnum(static_cast<unsigned char>(*current_char_)) || *current_char_ == '_')) {
        buffer_ += *current_char_;
        advance();
    }
    std::string tok = keywords_.value(buffer_, "IDENTIFIER");
    emit(tok, json(buffer_));
    state_ = State::Start;
}

inline void Lexer::state_number() {
    bool has_dot = false, has_exp = false;
    while (current_char_) {
        char c = *current_char_;
        if (std::isdigit(static_cast<unsigned char>(c))) { buffer_ += c; }
        else if (c == '.' && !has_dot) { has_dot = true; buffer_ += c; }
        else if ((c == 'e' || c == 'E') && !has_exp) {
            has_exp = true; buffer_ += c;
            auto nx = peek();
            if (nx && (*nx == '+' || *nx == '-')) { advance(); buffer_ += *current_char_; }
        } else { break; }
        advance();
    }
    if (has_dot || has_exp) emit("FLOAT", json(std::stod(buffer_)));
    else emit("INT", json(static_cast<int64_t>(std::stoll(buffer_))));
    state_ = State::Start;
}

inline void Lexer::state_string() {
    if (!current_char_) {
        std::ostringstream msg;
        msg << "Unclosed string at line " << line_ << ", col " << col_;
        handle_error("unclosed_string", msg.str());
        state_ = State::Start;
        return;
    }
    if (*current_char_ == '"') {
        emit("STRING", json(buffer_));
        advance();
        state_ = State::Start;
        return;
    }
    if (*current_char_ == '\\') {
        advance();
        char c = current_char_ ? *current_char_ : '\0';
        static const std::unordered_map<char, char> esc = {{'n', '\n'}, {'t', '\t'}, {'"', '"'}, {'\\', '\\'}};
        auto it = esc.find(c);
        buffer_ += (it != esc.end()) ? it->second : c;
    } else {
        buffer_ += *current_char_;
    }
    advance();
}

inline void Lexer::state_char() {
    if (!current_char_) {
        std::ostringstream msg;
        msg << "Unclosed char literal at line " << line_ << ", col " << col_;
        handle_error("unclosed_string", msg.str());
        state_ = State::Start;
        return;
    }
    std::string value(1, *current_char_);
    advance();
    if (!current_char_ || *current_char_ != '\'') {
        std::ostringstream msg;
        msg << "Invalid char literal at line " << line_ << ", col " << col_ << " -- expected closing \"'\"";
        handle_error("invalid_char", msg.str());
        state_ = State::Start;
        return;
    }
    advance();
    emit("CHAR", json(value));
    state_ = State::Start;
}

inline void Lexer::state_comment_line() {
    while (current_char_ && *current_char_ != '\n') advance();
    state_ = State::Start;
}

inline void Lexer::state_comment_block() {
    while (current_char_) {
        if (peek_str(2) == "*/") { advance(); advance(); break; }
        advance();
    }
    state_ = State::Start;
}

inline std::string detect_token_type(const Token& token) {
    const std::string& t = token.type;
    if (t == "INT") return "int";
    if (t == "FLOAT") return "float";
    if (t == "STRING") return "string";
    if (t == "CHAR") return ENABLE_CHAR_TYPE ? "char" : "string";
    if (t == "BOOL") return "bool";
    if (t == "IDENTIFIER") return "identifier";
    static const std::set<std::string> operators = {"PLUS", "MINUS", "MUL", "DIV", "ASSIGN", "EQ", "NEQ", "GT", "LT", "GTE", "LTE"};
    if (operators.count(t)) return "operator";
    static const std::set<std::string> brackets = {"LPAREN", "RPAREN", "LBRACKET", "RBRACKET", "LBRACE", "RBRACE"};
    if (brackets.count(t)) return "bracket";
    static const std::set<std::string> delims = {"COMMA", "SEMICOLON", "COLON"};
    if (delims.count(t)) return "delimiter";
    if (t == "NEWLINE") return "newline";
    if (t == "EOF") return "eof";
    std::string low = t;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return std::tolower(c); });
    return low;
}

inline json coerce_value(const Token& token) {
    if (token.type == "INT")   return json(token.value.get<int64_t>());
    if (token.type == "FLOAT") return json(token.value.get<double>());
    if (token.type == "BOOL")  return json(token.value.get<std::string>() == "true");
    return token.value;
}

inline json coerce_token(const Token& token, bool include_type) {
    json result;
    result["value"] = coerce_value(token);
    result["line"] = token.line;
    result["col"] = token.col;
    if (include_type) result["type"] = detect_token_type(token);
    return result;
}

inline std::string get_item_sep(const std::string& cmd_name) {
    auto it = COLLECT_DELIMITERS.find(cmd_name);
    if (it == COLLECT_DELIMITERS.end()) return "COMMA";
    auto j = it->second.find("item_sep");
    return j != it->second.end() ? j->second : "COMMA";
}

inline std::string get_kv_sep(const std::string& cmd_name) {
    auto it = COLLECT_DELIMITERS.find(cmd_name);
    if (it == COLLECT_DELIMITERS.end()) return "COLON";
    auto j = it->second.find("kv_sep");
    return j != it->second.end() ? j->second : "COLON";
}

inline json parse_list_tokens(const std::vector<Token>& raw_tokens, const std::string& open_type,
                        const std::string& close_type, const std::string& item_sep) {
    json result = json::array();
    std::vector<Token> current;
    int depth = 0;

    for (const Token& tok : raw_tokens) {
        if (tok.type == open_type) { ++depth; current.push_back(tok); }
        else if (tok.type == close_type) {
            --depth;
            if (depth < 0) {
                std::ostringstream msg;
                msg << "Unbalanced brackets in list at line " << tok.line << ", col " << tok.col;
                handle_error("unclosed_bracket", msg.str());
                break;
            }
            current.push_back(tok);
        } else if (tok.type == item_sep && depth == 0) {
            if (!current.empty()) result.push_back(eval_token_seq(current, open_type, close_type, item_sep));
            current.clear();
        } else {
            current.push_back(tok);
        }
    }
    if (!current.empty()) result.push_back(eval_token_seq(current, open_type, close_type, item_sep));
    return result;
}

inline json parse_map_tokens(const std::vector<Token>& raw_tokens, const std::string& open_type,
                       const std::string& close_type, const std::string& item_sep,
                       const std::string& kv_sep) {
    json result = json::object();
    int depth = 0;
    bool have_key = false;
    json key;
    std::vector<Token> val_tokens;
    enum { KEY, VAL } phase = KEY;

    auto flush = [&]() {
        if (have_key) {
            std::string key_str = key.is_string() ? key.get<std::string>() : key.dump();
            result[key_str] = eval_token_seq(val_tokens, "LBRACE", "RBRACE");
        }
        have_key = false; key = json(nullptr); val_tokens.clear(); phase = KEY;
    };

    for (const Token& tok : raw_tokens) {
        if (tok.type == "LBRACE" || tok.type == "LBRACKET") {
            ++depth;
            if (phase == VAL) val_tokens.push_back(tok);
        } else if (tok.type == "RBRACE" || tok.type == "RBRACKET") {
            --depth;
            if (phase == VAL) val_tokens.push_back(tok);
        } else if (tok.type == kv_sep && depth == 0 && phase == KEY) {
            phase = VAL;
        } else if (tok.type == item_sep && depth == 0) {
            flush();
        } else {
            if (phase == KEY) { key = coerce_value(tok); have_key = true; }
            else { val_tokens.push_back(tok); }
        }
    }
    flush();
    return result;
}

inline json eval_token_seq(const std::vector<Token>& tokens, const std::string& open_type,
                     const std::string& close_type, const std::string& item_sep) {
    if (tokens.empty()) return json(nullptr);
    if (tokens.front().type == open_type && tokens.back().type == close_type) {
        std::vector<Token> inner(tokens.begin() + 1, tokens.end() - 1);
        if (open_type == "LBRACKET") return parse_list_tokens(inner, open_type, close_type, item_sep);
        if (open_type == "LBRACE")   return parse_map_tokens(inner, open_type, close_type, item_sep);
    }
    if (tokens.size() == 1) return coerce_token(tokens[0]);
    json arr = json::array();
    for (auto& t : tokens) arr.push_back(coerce_token(t));
    return arr;
}

inline Normalizer::Normalizer(std::vector<Token> tokens, const json& grammar) {
    json cfg = grammar.value("_config", json::object());
    line_terminator_ = cfg.value("line_terminator", ";");
    rules_ = grammar.value("rules", json::object());
    stmt_separator_ = cfg.contains("stmt_separator") && !cfg["stmt_separator"].is_null()
                          ? std::optional<std::string>(cfg["stmt_separator"].get<std::string>())
                          : std::nullopt;
    sep_token_type_ = resolve_sep_type(stmt_separator_);

    ENABLE_CHAR_TYPE = cfg.value("enable_char_type", true);

    json raw_policy = grammar.value("_error_policy", json::object());
    error_policy_ = DEFAULT_ERROR_POLICY;
    for (auto& [k, v] : raw_policy.items()) error_policy_[k] = v.get<std::string>();

    bool keep_newline = cfg.value("keep_newline", false);
    std::set<std::string> exclude = keep_newline ? std::set<std::string>{"EOF"}
                                                   : std::set<std::string>{"NEWLINE", "EOF"};
    for (auto& t : tokens) if (!exclude.count(t.type)) tokens_.push_back(t);
    for (auto& t : tokens) if (t.type == "EOF") tokens_.push_back(t);
}

inline std::vector<json> Normalizer::parse() {
    while (true) {
        Token* tok = peek();
        if (!tok || tok->type == "EOF") break;

        if (!current_tab_) current_tab_ = tok->tab;
        int tab = *current_tab_;

        int stmt_line = tok->line, stmt_col = tok->col;
        std::string trigger_node = tok->type;

        if (!rules_.contains(tok->type)) {
            std::ostringstream msg;
            msg << "[Warning] No rule matched for token '" << tok->type
                << "' (value=" << tok->value.dump() << ") at " << loc(tok);
            handle_error("rule_not_matched", msg.str(), &error_policy_);
            consume();
            continue;
        }
        json rule = rules_.at(tok->type);

        last_collect_mode_.reset();
        size_t saved_pos = pos_;
        consume();
        std::vector<json> accumulated;
        auto result = walk_rule(rule, tab, accumulated);

        if (result) {
            auto& [action, data] = *result;
            std::string ir_type = resolve_ir_type(action);
            json data_arr = json::array();
            for (auto& d : data) data_arr.push_back(d);
            json data_type_info = resolve_data_type_detailed(last_collect_mode_, data_arr);

            json ir_node;
            ir_node["action"] = action;
            ir_node["type"] = ir_type;
            ir_node["node"] = trigger_node;
            ir_node["data_type"] = data_type_info;
            ir_node["data"] = data_arr;
            ir_node["tab"] = tab;
            ir_node["line"] = stmt_line;
            ir_node["col"] = stmt_col;
            ir_.push_back(ir_node);
        } else {
            pos_ = saved_pos;
            std::ostringstream msg;
            msg << "[Error] Rule '" << trigger_node << "' matched but produced no result at " << loc(tok);
            handle_error("rule_not_matched", msg.str(), &error_policy_);
            if (pos_ == saved_pos) consume();
        }
    }
    return ir_;
}

inline std::optional<std::string> Normalizer::resolve_sep_type(const std::optional<std::string>& sep_char) {
    if (!sep_char) return std::nullopt;
    static const std::unordered_map<std::string, std::string> rev = {
        {",", "COMMA"}, {";", "SEMICOLON"}, {":", "COLON"}, {"(", "LPAREN"}, {")", "RPAREN"},
        {"{", "LBRACE"}, {"}", "RBRACE"}, {"[", "LBRACKET"}, {"]", "RBRACKET"},
    };
    auto it = rev.find(*sep_char);
    return it != rev.end() ? std::optional<std::string>(it->second) : std::nullopt;
}

inline void Normalizer::err(const std::string& type, const std::string& message) {
    handle_error(type, message, &error_policy_);
}

inline Token* Normalizer::peek(int k) {
    size_t idx = pos_ + k;
    return idx < tokens_.size() ? &tokens_[idx] : nullptr;
}

inline Token Normalizer::consume() { return tokens_[pos_++]; }

inline std::string Normalizer::loc(const Token* tok) {
    if (!tok) return "end of input";
    std::ostringstream os; os << "line " << tok->line << ", col " << tok->col;
    return os.str();
}

inline bool Normalizer::is_terminator(const Token* tok) {
    if (!tok) return false;
    if (line_terminator_ == "NEWLINE") return tok->type == "NEWLINE";
    return tok->value.is_string() && tok->value.get<std::string>() == line_terminator_;
}

inline bool Normalizer::is_separator(const Token* tok) {
    if (!tok || !sep_token_type_) return false;
    if (*sep_token_type_ == line_terminator_) return false;
    return tok->type == *sep_token_type_;
}

inline std::string Normalizer::resolve_bracket_token_type(const json& spec) {
    if (spec.is_string()) return spec.get<std::string>();
    if (spec.is_object()) return spec.value("token", "UNKNOWN");
    return spec.dump();
}

inline json Normalizer::collect_until_close(const json& open_spec, const json& close_spec,
                                      const std::optional<std::set<std::string>>& allowed,
                                      const std::string& cmd_name) {
    std::string open_type = resolve_bracket_token_type(open_spec);
    std::string close_type = resolve_bracket_token_type(close_spec);

    Token* open_tok = peek();
    if (!open_tok || open_tok->type != open_type) {
        std::ostringstream msg;
        msg << "[SyntaxError] " << cmd_name << ": expected '" << open_type << "' but got "
            << (open_tok ? open_tok->repr() : "null") << " at " << loc(open_tok);
        err("unclosed_bracket", msg.str());
        return json::array();
    }
    Token open_tok_copy = *open_tok;
    consume();

    int depth = 1;
    bool is_map = (cmd_name == "collectmap");
    bool is_list = (cmd_name == "collectlist");
    std::vector<Token> raw_tokens;

    std::string item_sep = get_item_sep(cmd_name);
    std::string kv_sep = get_kv_sep(cmd_name);

    while (true) {
        Token* tok = peek();
        if (!tok || tok->type == "EOF") {
            std::ostringstream msg;
            msg << "[SyntaxError] " << cmd_name << ": unclosed '" << open_type
                << "' (opened at " << loc(&open_tok_copy) << ") -- reached end of input";
            err("unclosed_bracket", msg.str());
            break;
        }
        if (tok->type == open_type) { ++depth; raw_tokens.push_back(consume()); }
        else if (tok->type == close_type) {
            --depth;
            if (depth == 0) { consume(); break; }
            raw_tokens.push_back(consume());
        } else {
            if (allowed && !allowed->count(tok->type)) {
                std::ostringstream msg;
                msg << "[TypeError] " << cmd_name << ": unexpected token '" << tok->type
                    << "' (value=" << tok->value.dump() << ") at " << loc(tok);
                err("type_error", msg.str());
                consume();
                continue;
            }
            raw_tokens.push_back(consume());
        }
    }

    if (is_list) return parse_list_tokens(raw_tokens, open_type, close_type, item_sep);
    if (is_map)  return parse_map_tokens(raw_tokens, open_type, close_type, item_sep, kv_sep);
    json arr = json::array();
    for (auto& t : raw_tokens) arr.push_back(coerce_token(t));
    return arr;
}

inline json Normalizer::collect_code_block() {
    std::string open_type = "LBRACE", close_type = "RBRACE";
    auto it = COLLECT_BRACKETS.find("collectcode");
    if (it != COLLECT_BRACKETS.end() && it->second) {
        open_type = it->second->open_type; close_type = it->second->close_type;
    }

    Token* open_tok = peek();
    if (!open_tok || open_tok->type != open_type) {
        std::ostringstream msg;
        msg << "[SyntaxError] collectcode: expected '" << open_type << "' but got "
            << (open_tok ? open_tok->repr() : "null") << " at " << loc(open_tok);
        err("unclosed_bracket", msg.str());
        return json::array();
    }
    Token open_tok_copy = *open_tok;
    consume();

    int depth = 1;
    std::vector<Token> block_toks;
    while (true) {
        Token* tok = peek();
        if (!tok || tok->type == "EOF") {
            std::ostringstream msg;
            msg << "[SyntaxError] collectcode: unclosed '" << open_type
                << "' (opened at " << loc(&open_tok_copy) << ") -- reached end of input";
            err("unclosed_bracket", msg.str());
            break;
        }
        if (tok->type == open_type) { ++depth; block_toks.push_back(consume()); }
        else if (tok->type == close_type) {
            --depth;
            if (depth == 0) { consume(); break; }
            block_toks.push_back(consume());
        } else block_toks.push_back(consume());
    }

    Token eof_sentinel;
    if (!block_toks.empty()) {
        const Token& last = block_toks.back();
        eof_sentinel = Token{"EOF", json(nullptr), last.pos + 1, last.line, last.col + 1, last.tab};
    } else {
        eof_sentinel = Token{"EOF", json(nullptr), open_tok_copy.pos + 1,
                              open_tok_copy.line, open_tok_copy.col + 1, open_tok_copy.tab};
    }
    block_toks.push_back(eof_sentinel);

    json sub_grammar;
    sub_grammar["_config"]["line_terminator"] = line_terminator_;
    if (stmt_separator_) sub_grammar["_config"]["stmt_separator"] = *stmt_separator_;
    sub_grammar["rules"] = rules_;
    Normalizer sub(block_toks, sub_grammar);
    sub.error_policy_ = error_policy_;
    auto sub_ir = sub.parse();
    json arr = json::array();
    for (auto& n : sub_ir) arr.push_back(n);
    return arr;
}

inline json Normalizer::collect_expr(const std::string& cmd_name) {
    std::optional<std::set<std::string>> allowed;
    auto it = COLLECT_TYPES.find(cmd_name);
    if (it != COLLECT_TYPES.end()) allowed = it->second;

    static const std::set<std::string> expr_passthrough = {
        "LPAREN", "RPAREN", "PLUS", "MINUS", "MUL", "DIV", "EQ", "NEQ", "GT", "LT", "GTE", "LTE", "COMMA"
    };
    json data = json::array();
    int depth = 0;
    std::vector<Token> raw_tokens;

    while (true) {
        Token* tok = peek();
        if (!tok || tok->type == "EOF") break;
        if (depth == 0) {
            if (is_terminator(tok)) { consume(); current_tab_.reset(); break; }
            if (is_separator(tok)) { consume(); break; }
        }
        if (tok->type == "LPAREN") { ++depth; Token c = consume(); data.push_back(coerce_token(c)); raw_tokens.push_back(c); continue; }
        if (tok->type == "RPAREN") {
            if (depth == 0) break;
            --depth; Token c = consume(); data.push_back(coerce_token(c)); raw_tokens.push_back(c); continue;
        }
        if (allowed && !allowed->count(tok->type) && !expr_passthrough.count(tok->type)) {
            std::ostringstream msg;
            msg << "[TypeError] " << cmd_name << ": unexpected token '" << tok->type
                << "' (value=" << tok->value.dump() << ") at " << loc(tok);
            err("type_error", msg.str());
            consume();
            continue;
        }
        Token consumed = consume();
        data.push_back(coerce_token(consumed));
        raw_tokens.push_back(consumed);
    }

    if (depth != 0) {
        std::ostringstream msg;
        msg << "[SyntaxError] " << cmd_name << ": unbalanced parentheses near " << loc(peek());
        err("unclosed_bracket", msg.str());
    }

    if (cmd_name == "collectexpr") {
        std::ostringstream eq;
        for (size_t i = 0; i < raw_tokens.size(); ++i) {
            if (i) eq << " ";
            eq << (raw_tokens[i].value.is_string() ? raw_tokens[i].value.get<std::string>() : raw_tokens[i].value.dump());
        }
        json out;
        out["tokens"] = data;
        out["equation"] = eq.str();
        json raw_arr = json::array();
        for (auto& t : raw_tokens) raw_arr.push_back(coerce_token(t));
        out["raw_tokens"] = raw_arr;
        return out;
    }
    return data;
}

inline json Normalizer::collect_simple(const std::string& cmd_name) {
    std::optional<std::set<std::string>> allowed;
    auto it = COLLECT_TYPES.find(cmd_name);
    if (it != COLLECT_TYPES.end()) allowed = it->second;

    json data = json::array();
    while (true) {
        Token* tok = peek();
        if (!tok || tok->type == "EOF") break;
        if (is_terminator(tok)) { consume(); current_tab_.reset(); break; }
        if (is_separator(tok)) { consume(); break; }
        if (allowed && !allowed->count(tok->type)) {
            std::ostringstream msg;
            msg << "[TypeError] " << cmd_name << ": unexpected token '" << tok->type
                << "' (value=" << tok->value.dump() << ") at " << loc(tok);
            err("type_error", msg.str());
            consume();
            continue;
        }
        data.push_back(coerce_token(consume()));
    }
    return data;
}

inline json Normalizer::collect(const std::string& cmd_name, int /*tab*/) {
    last_collect_mode_ = cmd_name;

    if (cmd_name == "collectcode") return collect_code_block();
    if (COLLECT_EXPR_MODE.count(cmd_name)) return collect_expr(cmd_name);

    auto bit = COLLECT_BRACKETS.find(cmd_name);
    if (bit != COLLECT_BRACKETS.end() && bit->second) {
        std::optional<std::set<std::string>> allowed;
        auto tit = COLLECT_TYPES.find(cmd_name);
        if (tit != COLLECT_TYPES.end()) allowed = tit->second;
        return collect_until_close(json(bit->second->open_type), json(bit->second->close_type), allowed, cmd_name);
    }
    return collect_simple(cmd_name);
}

inline std::optional<std::pair<std::string, std::vector<json>>>
Normalizer::walk_rule(const json& rule_node, int tab, std::vector<json> accumulated) {
    if (rule_node.contains("alternatives")) {
        for (auto& alt : rule_node["alternatives"]) {
            size_t saved_pos = pos_;
            auto result = walk_rule(alt, tab, accumulated);
            if (result) return result;
            pos_ = saved_pos;
        }
        return std::nullopt;
    }

    for (auto it = rule_node.begin(); it != rule_node.end(); ++it) {
        const std::string& key = it.key();
        const json& child = it.value();

        if (key == "end") {
            std::string action = (child.is_array() && child.size() > 1) ? child[1].get<std::string>() : "Unknown_Cmd";
            Token* tok = peek();
            if (tok && is_terminator(tok)) { consume(); current_tab_.reset(); }
            else if (tok && is_separator(tok)) { consume(); }
            return std::make_pair(action, accumulated);
        }

        if (key == "collect") {
            Token* tok = peek();
            if (!tok || tok->type == "EOF") return std::nullopt;
            accumulated.push_back(coerce_token(consume()));
            return walk_rule(child, tab, accumulated);
        }

        if (COLLECT_TYPES.count(key)) {
            size_t saved_pos = pos_;
            json data;
            try {
                data = collect(key, tab);
            } catch (const std::runtime_error&) {
                pos_ = saved_pos;
                continue;
            } catch (const std::invalid_argument&) {
                pos_ = saved_pos;
                continue;
            }

            std::vector<json> combined = accumulated;
            if (key == "collectcode") {
                combined.push_back(data);
            } else if (data.is_array()) {
                for (auto& d : data) combined.push_back(d);
            } else {
                combined.push_back(data);
            }
            return walk_rule(child, tab, combined);
        }

        Token* tok = peek();
        if (!tok) continue;
        if (tok->type == key) {
            consume();
            size_t saved_pos = pos_;
            bool any_child = false;
            for (auto cit = child.begin(); cit != child.end(); ++cit) {
                any_child = true;
                pos_ = saved_pos;
                json single_child;
                single_child[cit.key()] = cit.value();
                auto result = walk_rule(single_child, tab, accumulated);
                if (result) return result;
            }
            (void)any_child;
            pos_ = saved_pos - 1;
            continue;
        }
    }
    return std::nullopt;
}

inline std::string get_data_type_string(const json& ir_node) {
    json dt = ir_node.value("data_type", json::object());
    if (dt.is_object()) return dt.value("value_type", dt.value("type", "unknown"));
    return dt.dump();
}

inline void print_ir_summary(const std::vector<json>& ir_list) {
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "IR SUMMARY WITH DATA TYPES\n";
    std::cout << std::string(70, '=') << "\n";
    int i = 1;
    for (auto& node : ir_list) {
        std::cout << "\n[" << i++ << "] Line " << node.value("line", 0) << ", Col " << node.value("col", 0) << "\n";
        std::cout << "    Action: " << node.value("action", "") << "\n";
        std::cout << "    Type: " << node.value("type", "") << "\n";
        std::cout << "    Node: " << node.value("node", "") << "\n";
        json dt = node.value("data_type", json::object());
        if (dt.is_object()) {
            std::cout << "    Data Type: " << dt.value("type", "") << "\n";
            std::cout << "    Value Type: " << dt.value("value_type", "") << "\n";
        }
        if (node.contains("data") && node["data"].is_object() && node["data"].contains("equation")) {
            std::cout << "    Equation: " << node["data"]["equation"].get<std::string>() << "\n";
        }
        std::cout << "    Data: " << node.value("data", json::array()).dump() << "\n";
    }
    std::cout << "\n" << std::string(70, '=') << "\n\n";
}
#endif // MONO_HPP