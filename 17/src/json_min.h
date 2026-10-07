// =====================================================================
// 17/ — 设备全点表与高保真模拟器 · 极小 JSON 解析/生成
//
// 为什么自己写（三条，与 P1/ 的 json_lite.h 同源，但本模块**不 include P1**）：
//   ① 项目纪律是 **header-only + 零第三方依赖**（约定 §1）。引入 nlohmann/json
//      会带来一个几百 KB 的头，且现场交叉编译最容易炸的就是第三方头。
//   ② 本模块要的只是 JSON 的一个**子集**：对象 / 数组 / 字符串 / 数字 / 布尔 / null。
//      不需要日期、不需要二进制、不需要流式解析。
//   ③ 最关键：C5 的判据要求"非法 JSON / 缺字段 → 显式报错并指出**第几个点、哪个字段**"。
//      自己写解析器才能把 **行:列** 稳定地带出来；第三方库的错误文本格式不受我们控制，
//      按它写断言就是"测别人的库"。
//
// ★ 为什么不直接 #include "../P1/src/json_lite.h"：
//   跨模块 include 在本仓库是允许的（约定 §1 说 include 是扁平的），但会让 `17/`
//   对 `P1/`（产品化配置模块）产生编译期依赖 —— 而 P1 的 include 路径、命名空间
//   都属于"另一个模块的私有实现"。17/ 要能**单独**编、单独验，所以这里自带一份。
//   两份实现的差异被断言钉住（见 tests/test_point_config.cpp 的往返用例）。
//
// 与严格 JSON 的三处**有意放宽**（配置文件是人手写的，不是机器生成的）：
//   ① 注释：`//` 行注释、`#` 行注释、`/* 块 */`
//   ② 尾逗号：`{"a":1,}` / `[1,2,]`
//   ③ 对象键可省略引号：`{soc_min: 0.1}`（仅限 [A-Za-z_][A-Za-z0-9_.]*）
//   现场调试想临时注释掉一行点位定义，不应该导致整张点表加载失败。
//
// 数字输出采用"最短可往返"策略（15/16/17 位有效数字逐级回退），
// 这样 dump→parse 的**双精度逐位相等**成立 —— C5 的往返幂等判据靠它。
//
// 编译：纯头文件，实现全部 inline。g++ 8.1 下**不用** <filesystem> / <charconv>。
// =====================================================================

#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace ems {
namespace jmin {

// =====================================================================
// 值
// =====================================================================
enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };

struct Value;
using Array  = std::vector<Value>;

// 对象用**有序** vector 而不是 map：
//   · 往返一致（dump(parse(x)) 与 x 的键序一致）
//   · 点表导出成 JSON 后，点位顺序就是点表顺序，diff 友好
using Object = std::vector<std::pair<std::string, Value>>;

struct Value {
    Type        type = Type::kNull;
    bool        b    = false;
    double      num  = 0.0;
    std::string str;
    Array       arr;
    Object      obj;

    Value() = default;
    explicit Value(bool v)        : type(Type::kBool), b(v) {}
    explicit Value(double v)      : type(Type::kNumber), num(v) {}
    explicit Value(int v)         : type(Type::kNumber), num(static_cast<double>(v)) {}
    explicit Value(const char* s) : type(Type::kString), str(s ? s : "") {}
    explicit Value(std::string s) : type(Type::kString), str(std::move(s)) {}

    static Value make_object() { Value v; v.type = Type::kObject; return v; }
    static Value make_array()  { Value v; v.type = Type::kArray;  return v; }

    bool is_null()   const { return type == Type::kNull; }
    bool is_bool()   const { return type == Type::kBool; }
    bool is_number() const { return type == Type::kNumber; }
    bool is_string() const { return type == Type::kString; }
    bool is_array()  const { return type == Type::kArray; }
    bool is_object() const { return type == Type::kObject; }

    const Value* find(const std::string& key) const {
        if (type != Type::kObject) return nullptr;
        for (const auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    Value* find(const std::string& key) {
        if (type != Type::kObject) return nullptr;
        for (auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    bool has(const std::string& key) const { return find(key) != nullptr; }

    void set(const std::string& key, Value v) {
        if (type != Type::kObject) { type = Type::kObject; obj.clear(); }
        if (Value* p = find(key)) { *p = std::move(v); return; }
        obj.emplace_back(key, std::move(v));
    }
    void push(Value v) {
        if (type != Type::kArray) { type = Type::kArray; arr.clear(); }
        arr.push_back(std::move(v));
    }

    double number(double d = 0.0) const { return is_number() ? num : d; }
    bool   boolean(bool d = false) const { return is_bool() ? b : d; }
    std::string string(const std::string& d = "") const {
        return is_string() ? str : d;
    }
    std::size_t size() const {
        if (type == Type::kArray)  return arr.size();
        if (type == Type::kObject) return obj.size();
        return 0;
    }

    const char* type_name() const {
        switch (type) {
            case Type::kNull:   return "null";
            case Type::kBool:   return "bool";
            case Type::kNumber: return "number";
            case Type::kString: return "string";
            case Type::kArray:  return "array";
            case Type::kObject: return "object";
        }
        return "?";
    }
};

// =====================================================================
// 解析错误（带行:列 —— 配置错误的第一现场就是"人看文件找问题"）
// =====================================================================
struct ParseError {
    bool        ok = true;
    std::string message;
    int         line = 1;
    int         col  = 1;

    std::string to_string() const {
        if (ok) return "ok";
        char buf[64];
        std::snprintf(buf, sizeof(buf), "JSON 解析错误 %d:%d —— ", line, col);
        return std::string(buf) + message;
    }
};

// =====================================================================
// 解析器（递归下降）
// =====================================================================
namespace detail {

class Parser {
public:
    Parser(const std::string& s, ParseError* err) : s_(s), err_(err) {}

    bool parse(Value* out) {
        skip_ws();
        if (!parse_value(out)) return false;
        skip_ws();
        if (i_ != s_.size()) return fail("文件末尾存在多余内容");
        return true;
    }

private:
    const std::string& s_;
    ParseError*        err_  = nullptr;
    std::size_t        i_    = 0;
    int                line_ = 1;
    int                col_  = 1;

    bool eof() const { return i_ >= s_.size(); }
    char peek() const { return eof() ? '\0' : s_[i_]; }

    char advance() {
        if (eof()) return '\0';
        const char c = s_[i_++];
        if (c == '\n') { ++line_; col_ = 1; } else { ++col_; }
        return c;
    }

    bool fail(const std::string& msg) {
        if (err_ && err_->ok) {
            err_->ok      = false;
            err_->message = msg;
            err_->line    = line_;
            err_->col     = col_;
        }
        return false;
    }

    void skip_ws() {
        for (;;) {
            while (!eof()) {
                const char c = peek();
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n') advance();
                else break;
            }
            if (eof()) return;
            if (peek() == '#') {                                  // # 行注释
                while (!eof() && peek() != '\n') advance();
                continue;
            }
            if (peek() == '/' && i_ + 1 < s_.size()) {
                const char n = s_[i_ + 1];
                if (n == '/') {                                   // // 行注释
                    while (!eof() && peek() != '\n') advance();
                    continue;
                }
                if (n == '*') {                                   // /* 块注释 */
                    advance(); advance();
                    bool closed = false;
                    while (!eof()) {
                        if (peek() == '*' && i_ + 1 < s_.size() && s_[i_ + 1] == '/') {
                            advance(); advance(); closed = true; break;
                        }
                        advance();
                    }
                    if (!closed) { fail("块注释未闭合"); return; }
                    continue;
                }
            }
            return;
        }
    }

    static bool is_ident_start(char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    }
    static bool is_ident_char(char c) {
        return is_ident_start(c) || (c >= '0' && c <= '9') || c == '.' || c == '-';
    }

    bool parse_value(Value* out) {
        skip_ws();
        if (eof()) return fail("期待一个值，但已到文件末尾");
        const char c = peek();
        if (c == '{') return parse_object(out);
        if (c == '[') return parse_array(out);
        if (c == '"') { out->type = Type::kString; return parse_string(&out->str); }
        if (c == 't' || c == 'f') return parse_bool(out);
        if (c == 'n') {
            if (s_.compare(i_, 4, "null") == 0) {
                for (int k = 0; k < 4; ++k) advance();
                *out = Value();
                return true;
            }
            return fail("非法的字面量（期待 null）");
        }
        if (c == '-' || c == '+' || (c >= '0' && c <= '9')) return parse_number(out);
        if (is_ident_start(c)) {                     // 裸标识符当字符串
            std::string id;
            while (!eof() && is_ident_char(peek())) id.push_back(advance());
            out->type = Type::kString;
            out->str  = id;
            return true;
        }
        return fail(std::string("非法的值起始字符 '") + c + "'");
    }

    bool parse_bool(Value* out) {
        if (s_.compare(i_, 4, "true") == 0) {
            for (int k = 0; k < 4; ++k) advance();
            *out = Value(true);
            return true;
        }
        if (s_.compare(i_, 5, "false") == 0) {
            for (int k = 0; k < 5; ++k) advance();
            *out = Value(false);
            return true;
        }
        return fail("非法的字面量（期待 true / false）");
    }

    bool parse_number(Value* out) {
        const std::size_t start = i_;
        if (peek() == '-' || peek() == '+') advance();
        bool any_digit = false;
        while (!eof() && peek() >= '0' && peek() <= '9') { advance(); any_digit = true; }
        if (!eof() && peek() == '.') {
            advance();
            while (!eof() && peek() >= '0' && peek() <= '9') { advance(); any_digit = true; }
        }
        if (any_digit && !eof() && (peek() == 'e' || peek() == 'E')) {
            const std::size_t save = i_;
            const int sl = line_, sc = col_;
            advance();
            if (peek() == '-' || peek() == '+') advance();
            bool exp_digit = false;
            while (!eof() && peek() >= '0' && peek() <= '9') { advance(); exp_digit = true; }
            if (!exp_digit) { i_ = save; line_ = sl; col_ = sc; }   // 回退（1e 后面没数字）
        }
        if (!any_digit) return fail("非法的数字");
        const std::string tok = s_.substr(start, i_ - start);
        // 不用 std::stod：它抛异常（异常在 -O2 下也能用，但这里用 strtod 更直白）
        char*  endp = nullptr;
        const double v = std::strtod(tok.c_str(), &endp);
        if (endp == tok.c_str() || *endp != '\0') return fail("数字无法解析: " + tok);
        out->type = Type::kNumber;
        out->num  = v;
        return true;
    }

    bool parse_string(std::string* out) {
        if (peek() != '"') return fail("期待 '\"'");
        advance();
        out->clear();
        for (;;) {
            if (eof()) return fail("字符串未闭合");
            const char c = advance();
            if (c == '"') return true;
            if (c != '\\') { out->push_back(c); continue; }
            if (eof()) return fail("转义序列不完整");
            const char e = advance();
            switch (e) {
                case '"':  out->push_back('"');  break;
                case '\\': out->push_back('\\'); break;
                case '/':  out->push_back('/');  break;
                case 'b':  out->push_back('\b'); break;
                case 'f':  out->push_back('\f'); break;
                case 'n':  out->push_back('\n'); break;
                case 'r':  out->push_back('\r'); break;
                case 't':  out->push_back('\t'); break;
                case 'u': {
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        if (eof()) return fail("\\u 转义不完整");
                        const char h = advance();
                        cp <<= 4;
                        if (h >= '0' && h <= '9')      cp |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                        else return fail("\\u 转义含非法十六进制字符");
                    }
                    // 只做 UTF-8 编码（不处理代理对：点表里不会出现）
                    if (cp < 0x80) {
                        out->push_back(static_cast<char>(cp));
                    } else if (cp < 0x800) {
                        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: return fail(std::string("未知转义 \\") + e);
            }
        }
    }

    bool parse_key(std::string* out) {
        skip_ws();
        if (peek() == '"') return parse_string(out);
        if (!is_ident_start(peek())) return fail("期待对象键");
        out->clear();
        while (!eof() && is_ident_char(peek())) out->push_back(advance());
        return true;
    }

    bool parse_object(Value* out) {
        advance();                                  // '{'
        out->type = Type::kObject;
        out->obj.clear();
        skip_ws();
        if (peek() == '}') { advance(); return true; }
        for (;;) {
            std::string key;
            if (!parse_key(&key)) return false;
            skip_ws();
            if (peek() != ':') return fail("对象键后期待 ':'");
            advance();
            Value v;
            if (!parse_value(&v)) return false;
            out->obj.emplace_back(std::move(key), std::move(v));
            skip_ws();
            if (peek() == ',') { advance(); skip_ws(); if (peek() == '}') { advance(); return true; } continue; }
            if (peek() == '}') { advance(); return true; }
            return fail("对象中期待 ',' 或 '}'");
        }
    }

    bool parse_array(Value* out) {
        advance();                                  // '['
        out->type = Type::kArray;
        out->arr.clear();
        skip_ws();
        if (peek() == ']') { advance(); return true; }
        for (;;) {
            Value v;
            if (!parse_value(&v)) return false;
            out->arr.push_back(std::move(v));
            skip_ws();
            if (peek() == ',') { advance(); skip_ws(); if (peek() == ']') { advance(); return true; } continue; }
            if (peek() == ']') { advance(); return true; }
            return fail("数组中期待 ',' 或 ']'");
        }
    }
};

} // namespace detail

// =====================================================================
// 输出
// =====================================================================
inline std::string escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    o.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                } else {
                    o.push_back(c);
                }
        }
    }
    o.push_back('"');
    return o;
}

// 数字 → 字符串：**最短可往返**
//   整数值 → "%.0f"（1e9 会变 1000000000，可读；且 strtod 逐位回得去）
//   否则依次试 %.15g / %.16g / %.17g，取第一个能 strtod 回来的
//
// ★ 为什么必须这么较真：C5 判据①是"导出 JSON 再加载回来逐点逐字段完全一致"，
//   而"逐字段一致"里包含 double。若用 %.6g 输出 0.95 与用 %.17g 输出的
//   0.94999999999999996 都"看起来对"，但只有最短可往返那种能让**逐位相等**成立。
inline std::string num_to_string(double v) {
    if (!std::isfinite(v)) return "null";
    const double r = std::floor(v);
    if (r == v && std::fabs(v) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f", v);
        return buf;
    }
    const char* fmts[3] = {"%.15g", "%.16g", "%.17g"};
    for (int i = 0; i < 3; ++i) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), fmts[i], v);
        if (std::strtod(buf, nullptr) == v) return buf;
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

namespace detail {

inline void dump_into(const Value& v, std::string* o, int indent, int depth) {
    const std::string pad(static_cast<std::size_t>(indent) * depth, ' ');
    const std::string pad1(static_cast<std::size_t>(indent) * (depth + 1), ' ');
    switch (v.type) {
        case Type::kNull:   *o += "null"; break;
        case Type::kBool:   *o += v.b ? "true" : "false"; break;
        case Type::kNumber: *o += num_to_string(v.num); break;
        case Type::kString: *o += escape(v.str); break;
        case Type::kArray: {
            if (v.arr.empty()) { *o += "[]"; break; }
            // "扁平数组"（元素全为标量）一行打完 —— 点表的 range 是 [min,max]，
            // 展开成 4 行会让 107 点的配置文件多出 300 行噪声。
            bool flat = true;
            for (const auto& e : v.arr) {
                if (e.type == Type::kArray || e.type == Type::kObject) { flat = false; break; }
            }
            if (flat && indent != 0) {
                *o += "[";
                for (std::size_t i = 0; i < v.arr.size(); ++i) {
                    if (i) *o += ", ";
                    dump_into(v.arr[i], o, indent, depth + 1);
                }
                *o += "]";
                break;
            }
            if (indent == 0) {
                *o += "[";
                for (std::size_t i = 0; i < v.arr.size(); ++i) {
                    if (i) *o += ",";
                    dump_into(v.arr[i], o, indent, depth + 1);
                }
                *o += "]";
                break;
            }
            *o += "[\n";
            for (std::size_t i = 0; i < v.arr.size(); ++i) {
                *o += pad1;
                dump_into(v.arr[i], o, indent, depth + 1);
                if (i + 1 < v.arr.size()) *o += ",";
                *o += "\n";
            }
            *o += pad + "]";
            break;
        }
        case Type::kObject:
            if (v.obj.empty()) { *o += "{}"; break; }
            if (indent == 0) {
                *o += "{";
                for (std::size_t i = 0; i < v.obj.size(); ++i) {
                    if (i) *o += ",";
                    *o += escape(v.obj[i].first) + ":";
                    dump_into(v.obj[i].second, o, indent, depth + 1);
                }
                *o += "}";
                break;
            }
            *o += "{\n";
            for (std::size_t i = 0; i < v.obj.size(); ++i) {
                *o += pad1 + escape(v.obj[i].first) + ": ";
                dump_into(v.obj[i].second, o, indent, depth + 1);
                if (i + 1 < v.obj.size()) *o += ",";
                *o += "\n";
            }
            *o += pad + "}";
            break;
    }
}

} // namespace detail

// =====================================================================
// 公开接口
// =====================================================================
inline Value parse(const std::string& text, ParseError* err = nullptr) {
    ParseError local;
    ParseError* e = err ? err : &local;
    e->ok = true;
    e->message.clear();
    e->line = 1;
    e->col  = 1;
    Value v;
    detail::Parser p(text, e);
    if (!p.parse(&v)) return Value();      // 失败返回 null（调用方看 err）
    return v;
}

inline std::string dump(const Value& v, int indent = 2) {
    std::string o;
    detail::dump_into(v, &o, indent, 0);
    o.push_back('\n');
    return o;
}

} // namespace jmin
} // namespace ems
