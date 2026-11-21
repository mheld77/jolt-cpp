// SPDX-License-Identifier: Apache-2.0
#include "modifier.hpp"
#include "jolt/jolt.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unicode/locid.h>
#include <unicode/regex.h>
#include <unicode/unistr.h>
#include <vector>

namespace jolt::detail {
namespace {

using Result = std::optional<Json>;
using Args = std::vector<Json>;
using Handler = Result (*)(const Args&);
constexpr std::size_t max_index = 100000;
constexpr std::size_t max_string = 4 * 1024 * 1024;

std::string_view trim(std::string_view text) {
    auto first = text.find_first_not_of(" \t\r\n");
    if (first == text.npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

template<class Number>
std::optional<Number> read_number(std::string_view text) {
    if (text.empty()) return {};
    if (text.front() == '+') text.remove_prefix(1);
    if (text.empty()) return {};
    Number result{};
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size()) return {};
    if constexpr (std::is_floating_point_v<Number>) {
        if (!std::isfinite(result)) return {};
    }
    return result;
}

std::string java_text(const Json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (!value.is_structured()) return value.dump();
    const bool object = value.is_object();
    std::string text = object ? "{" : "[";
    for (auto cursor = value.begin(); cursor != value.end(); ++cursor) {
        if (cursor != value.begin()) text += ", ";
        if (object) text += cursor.key() + "=";
        text += java_text(*cursor);
        if (text.size() > max_string) throw LimitExceeded("Converted string exceeds the size limit");
    }
    text += object ? "}" : "]";
    return text;
}

icu::UnicodeString unicode(const Json& value) {
    return icu::UnicodeString::fromUTF8(value.get_ref<const std::string&>());
}

Json utf8(const icu::UnicodeString& value) {
    std::string out;
    value.toUTF8String(out);
    return out;
}

std::string join(const Args& args, std::size_t start, std::string_view delimiter) {
    std::string out;
    for (auto i = start; i < args.size(); ++i) {
        if (args[i].is_null()) continue;
        auto text = java_text(args[i]);
        if (!delimiter.empty() && text.empty()) continue;
        if (out.size() + text.size() + delimiter.size() > max_string)
            throw LimitExceeded("Joined string exceeds the size limit");
        out += text;
        if (i + 1 < args.size()) out += delimiter;
    }
    return out;
}

std::optional<double> numeric(const Json& value) {
    if (value.is_number()) return value.get<double>();
    return value.is_string() ? read_number<double>(trim(value.get_ref<const std::string&>())) : std::nullopt;
}

template<class Integer>
std::optional<Integer> integer(const Json& value) {
    if (value.is_string()) {
        auto text = value.get_ref<const std::string&>();
        if (auto exact = read_number<std::int64_t>(text)) return static_cast<Integer>(*exact);
        auto converted = read_number<double>(trim(text));
        return converted ? integer<Integer>(Json(*converted)) : std::nullopt;
    }
    if (!value.is_number()) return {};
    if (value.is_number_unsigned()) {
        auto n = value.get<std::uint64_t>();
        return static_cast<Integer>(n);
    }
    if (value.is_number_integer()) {
        auto n = value.get<std::int64_t>();
        return static_cast<Integer>(n);
    }
    auto n = std::trunc(value.get<double>());
    if (n <= static_cast<double>(std::numeric_limits<Integer>::min())) return std::numeric_limits<Integer>::min();
    if (n >= -static_cast<double>(std::numeric_limits<Integer>::min())) return std::numeric_limits<Integer>::max();
    return static_cast<Integer>(n);
}

std::optional<bool> boolean(const Json& value) {
    if (value.is_boolean()) return value.get<bool>();
    if (!value.is_string()) return {};
    auto text = unicode(value);
    text.toLower(icu::Locale::getRoot());
    if (text == "true") return true;
    if (text == "false") return false;
    return {};
}

template<class Function>
Result on_value(const Json& source, Function function) {
    if (!source.is_array()) return function(source);
    auto result = source;
    for (auto& item : result) {
        if (auto converted = function(item)) item = std::move(*converted);
    }
    return result;
}

template<class Number>
Result value_result(std::optional<Number> value) {
    if (!value) return {};
    return Json(*value);
}

Result split(const Args& args) {
    if (args.size() != 2 || !args[0].is_string() || !args[1].is_string()) return {};
    auto source = unicode(args[1]);
    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::RegexPattern> pattern(icu::RegexPattern::compile(unicode(args[0]), 0, status));
    if (U_FAILURE(status)) return {};
    std::unique_ptr<icu::RegexMatcher> matches(pattern->matcher(source, status));
    if (U_FAILURE(status)) return {};
    matches->setTimeLimit(25, status);
    matches->setStackLimit(1024 * 1024, status);
    Json result = Json::array();
    int32_t start = 0;
    bool matched = false;
    while (matches->find(status)) {
        const auto first = matches->start(status);
        const auto last = matches->end(status);
        if (first == 0 && last == 0) continue;
        if (result.size() == max_index) throw LimitExceeded("Split output limit exceeded");
        matched = true;
        result.push_back(utf8(source.tempSubStringBetween(start, first)));
        start = last;
    }
    if (status == U_REGEX_TIME_OUT || status == U_REGEX_STACK_OVERFLOW)
        throw LimitExceeded("Split expression exceeds the execution limit");
    if (U_FAILURE(status)) return {};
    result.push_back(utf8(source.tempSubString(start)));
    while (matched && !result.empty() && result.back() == "") result.erase(result.size() - 1);
    return result;
}

Result text_case(const Args& args, bool upper) {
    if (args.size() != 1) return {};
    return on_value(args[0], [upper](const Json& value) -> Result {
        if (!value.is_string()) return {};
        auto text = unicode(value);
        if (upper) text.toUpper(icu::Locale::getRoot()); else text.toLower(icu::Locale::getRoot());
        return utf8(text);
    });
}

Result trim_text(const Args& args) {
    if (args.size() != 1) return {};
    return on_value(args[0], [](const Json& value) -> Result {
        if (!value.is_string()) return {};
        const auto& text = value.get_ref<const std::string&>();
        auto first = text.begin();
        auto last = text.end();
        while (first != last && static_cast<unsigned char>(*first) <= 0x20) ++first;
        while (first != last && static_cast<unsigned char>(*(last - 1)) <= 0x20) --last;
        return Json(std::string(first, last));
    });
}

Result substring(const Args& args) {
    if (args.size() != 3 || !args[0].is_string() || !args[1].is_number_integer() ||
        !args[2].is_number_integer()) return {};
    auto start = integer<int32_t>(args[1]);
    auto end = integer<int32_t>(args[2]);
    auto source = unicode(args[0]);
    if (!start || !end || *start < 0 || *start >= *end || *end > source.length()) return {};
    return utf8(source.tempSubStringBetween(*start, *end));
}

Result pad(const Args& args, bool left) {
    if (args.size() != 3 || !args[0].is_string() || !args[1].is_number_integer() ||
        !args[2].is_string()) return {};
    auto width = integer<int32_t>(args[1]);
    auto filler = unicode(args[2]);
    if (!width || *width <= 0 || *width > 500 || filler.length() != 1) return {};
    auto text = unicode(args[0]);
    if (*width <= text.length()) return args[0];
    icu::UnicodeString padding;
    for (auto i = text.length(); i < *width; ++i) padding.append(filler);
    return utf8(left ? padding.append(text) : text.append(padding));
}

template<class Conversion>
Result convert(const Args& args, Conversion convert_value) {
    return args.size() == 1 ? on_value(args[0], convert_value) : Result{};
}

void squash(Json& value, bool recursive) {
    if (!value.is_structured()) return;
    if (value.is_array()) {
        for (auto i = value.size(); i > 0; --i) {
            if (value[i - 1].is_null()) value.erase(i - 1);
            else if (recursive) squash(value[i - 1], true);
        }
    } else {
        for (auto it = value.begin(); it != value.end();) {
            if (it->is_null()) it = value.erase(it);
            else {
                if (recursive) squash(*it, true);
                ++it;
            }
        }
    }
}

const Args effective_args(const Args& args) {
    if (args.size() == 1 && args[0].is_array()) return args[0].get<Args>();
    return args;
}

enum class Aggregate { min, max, avg, sum, subtract };

template<class Number>
Result aggregate(const Args& args, Aggregate operation) {
    auto input = effective_args(args);
    std::optional<Number> total;
    std::size_t count = 0;
    for (const auto& item : input) {
        auto number = [&] {
            if constexpr (std::is_integral_v<Number>) return integer<Number>(item);
            else return numeric(item);
        }();
        if (!number) continue;
        ++count;
        if (!total) total = *number;
        else if (operation == Aggregate::min) total = std::min(*total, *number);
        else if (operation == Aggregate::max) total = std::max(*total, *number);
        else {
            if constexpr (std::is_integral_v<Number>) {
                // Java integer arithmetic wraps at the declared width.
                using Unsigned = std::make_unsigned_t<Number>;
                auto lhs = static_cast<Unsigned>(*total);
                auto rhs = static_cast<Unsigned>(*number);
                total = static_cast<Number>(operation == Aggregate::subtract ? lhs - rhs : lhs + rhs);
            } else {
                *total += operation == Aggregate::subtract ? -*number : *number;
            }
        }
    }
    if (!total) return {};
    if (operation == Aggregate::avg) {
        return Json(static_cast<double>(*total) / static_cast<double>(count));
    }
    if constexpr (std::is_floating_point_v<Number>) {
        if (!std::isfinite(*total)) return {};
    }
    return Json(*total);
}

Result divide(const Args& args, bool rounded) {
    auto base = rounded ? 1u : 0u;
    if (args.size() != base + 2) return {};
    auto lhs = numeric(args[base]), rhs = numeric(args[base + 1]);
    if (!lhs || !rhs || *rhs == 0) return {};
    auto value = *lhs / *rhs;
    if (rounded) {
        auto places = integer<int>(args[0]);
        if (!places || *places < 0 || *places > 15) return {};
        auto scale = std::pow(10.0, *places);
        value = std::round(value * scale) / scale;
    }
    if (!std::isfinite(value)) return {};
    return Json(value);
}

Result list_element(const Args& args, int position) {
    auto list = effective_args(args);
    if (list.empty()) return {};
    auto index = position < 0 ? list.size() - 1 : static_cast<std::size_t>(position);
    return index < list.size() ? Result(list[index]) : Result{};
}

struct Function {
    std::string_view name;
    Handler apply;
};

constexpr Function functions[] = {
    {"concat", [](const Args& a) -> Result { return Json(join(a, 0, {})); }},
    {"join", [](const Args& a) -> Result {
        if (a.size() < 2 || !a[0].is_string()) return {};
        return Json(join(a, 1, a[0].get_ref<const std::string&>()));
    }},
    {"toUpper", [](const Args& a) { return text_case(a, true); }},
    {"toLower", [](const Args& a) { return text_case(a, false); }},
    {"trim", trim_text}, {"substring", substring}, {"split", split},
    {"leftPad", [](const Args& a) { return pad(a, true); }},
    {"rightPad", [](const Args& a) { return pad(a, false); }},
    {"toInteger", [](const Args& a) { return convert(a, [](const Json& v) { return value_result(integer<int32_t>(v)); }); }},
    {"toLong", [](const Args& a) { return convert(a, [](const Json& v) { return value_result(integer<int64_t>(v)); }); }},
    {"toDouble", [](const Args& a) { return convert(a, [](const Json& v) { return value_result(numeric(v)); }); }},
    {"toBoolean", [](const Args& a) { return convert(a, [](const Json& v) { return value_result(boolean(v)); }); }},
    {"toString", [](const Args& a) {
        return convert(a, [](const Json& v) -> Result { return v.is_null() ? Result{} : Json(java_text(v)); });
    }},
    {"size", [](const Args& a) -> Result {
        if (a.empty()) return {};
        if (a.size() > 1) return Json(a.size());
        if (a[0].is_string()) return Json(unicode(a[0]).length());
        if (a[0].is_structured()) return Json(a[0].size());
        return {};
    }},
    {"noop", [](const Args&) -> Result { return {}; }},
    {"isPresent", [](const Args& a) -> Result { return a.empty() ? Result{} : a.front(); }},
    {"notNull", [](const Args& a) -> Result { return a.empty() || a.front().is_null() ? Result{} : a.front(); }},
    {"isNull", [](const Args& a) -> Result { return !a.empty() && a.front().is_null() ? Result(Json(nullptr)) : Result{}; }},
    {"firstElement", [](const Args& a) { return list_element(a, 0); }},
    {"lastElement", [](const Args& a) { return list_element(a, -1); }},
    {"elementAt", [](const Args& a) -> Result {
        if (a.size() < 2) return {};
        auto index = integer<int>(a.front());
        if (!index || *index < 0) return {};
        return list_element(Args(a.begin() + 1, a.end()), *index);
    }},
    {"toList", [](const Args& a) -> Result { return Json(effective_args(a)); }},
    {"sort", [](const Args& a) -> Result {
        auto values = effective_args(a);
        if (!std::all_of(values.begin(), values.end(), [](const Json& v) { return v.is_number(); }) &&
            !std::all_of(values.begin(), values.end(), [](const Json& v) { return v.is_string(); })) return {};
        std::stable_sort(values.begin(), values.end(), [](const Json& a, const Json& b) {
            return a.is_string() ? unicode(a).compare(unicode(b)) < 0 : a < b;
        });
        return Json(values);
    }},
    {"squashNulls", [](const Args& a) -> Result {
        if (a.size() != 1) return {};
        auto value = a.front();
        squash(value, false);
        return value;
    }},
    {"recursivelySquashNulls", [](const Args& a) -> Result {
        if (a.size() != 1) return {};
        auto value = a.front();
        squash(value, true);
        return value;
    }},
    {"min", [](const Args& a) { return aggregate<double>(a, Aggregate::min); }},
    {"max", [](const Args& a) { return aggregate<double>(a, Aggregate::max); }},
    {"avg", [](const Args& a) { return aggregate<double>(a, Aggregate::avg); }},
    {"intSum", [](const Args& a) { return aggregate<int32_t>(a, Aggregate::sum); }},
    {"longSum", [](const Args& a) { return aggregate<int64_t>(a, Aggregate::sum); }},
    {"doubleSum", [](const Args& a) { return aggregate<double>(a, Aggregate::sum); }},
    {"intSubtract", [](const Args& a) { return aggregate<int32_t>(a, Aggregate::subtract); }},
    {"longSubtract", [](const Args& a) { return aggregate<int64_t>(a, Aggregate::subtract); }},
    {"doubleSubtract", [](const Args& a) { return aggregate<double>(a, Aggregate::subtract); }},
    {"divide", [](const Args& a) { return divide(a, false); }},
    {"divideAndRound", [](const Args& a) { return divide(a, true); }},
    {"abs", [](const Args& a) {
        return convert(a, [](const Json& v) -> Result {
            auto value = numeric(v);
            return value ? Result(Json(std::abs(*value))) : Result{};
        });
    }}
};

std::vector<std::string_view> arguments(std::string_view text) {
    std::vector<std::string_view> result;
    if (trim(text).empty()) return result;
    std::size_t start = 0;
    int depth = 0;
    char quoted = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char ch = text[i];
        if (quoted) {
            if (ch == '\\') ++i;
            else if (ch == quoted) quoted = 0;
        } else if (ch == '\'' || ch == '"') quoted = ch;
        else if (ch == '(' || ch == '[') ++depth;
        else if (ch == ')' || ch == ']') --depth;
        else if (ch == ',' && depth == 0) {
            result.push_back(trim(text.substr(start, i - start)));
            start = i + 1;
        }
        if (depth < 0) throw Invalid("Unbalanced modifier arguments");
    }
    if (depth || quoted) throw Invalid("Unbalanced modifier arguments");
    result.push_back(trim(text.substr(start)));
    return result;
}

struct Scope {
    const Json& context;
    std::vector<const Json*> values;
    std::vector<std::string> keys;
    std::size_t& remaining;

    void spend(std::size_t cost = 1) const {
        if (cost > remaining) throw LimitExceeded("Modifier work limit exceeded");
        remaining -= cost;
    }
};

std::string key_reference(std::string_view key, const Scope& scope) {
    if (!key.starts_with('&')) return std::string(key);
    key.remove_prefix(1);
    auto offset = key.empty() ? std::optional<std::size_t>(0) : read_number<std::size_t>(key);
    if (!offset || *offset >= scope.keys.size()) throw Invalid("Invalid modifier key reference");
    return scope.keys[scope.keys.size() - 1 - *offset];
}

const Json* lookup(const Json* node, std::string_view path, const Scope& scope) {
    for (std::size_t offset = 0; node && offset < path.size();) {
        if (path[offset] == '.') {
            ++offset;
        } else if (path[offset] == '[') {
            auto close = path.find(']', offset + 1);
            if (close == path.npos) throw Invalid("Unclosed array reference");
            auto index = read_number<std::size_t>(key_reference(path.substr(offset + 1, close - offset - 1), scope));
            if (!index || !node->is_array() || *index >= node->size()) return nullptr;
            node = &(*node)[*index];
            offset = close + 1;
        } else {
            auto stop = path.find_first_of(".[", offset);
            auto key = key_reference(path.substr(offset, stop == path.npos ? path.npos : stop - offset), scope);
            if (node->is_object()) {
                auto child = node->find(key);
                node = child == node->end() ? nullptr : &*child;
            } else if (node->is_array()) {
                auto index = read_number<std::size_t>(key);
                node = index && *index < node->size() ? &(*node)[*index] : nullptr;
            } else return nullptr;
            if (stop == path.npos) break;
            offset = stop;
        }
    }
    return node;
}

Result evaluate(std::string_view expr, const Scope& scope, bool argument = false, unsigned depth = 0);

Result evaluate_function(std::string_view expr, const Scope& scope, unsigned depth) {
    auto open = expr.find('(');
    auto name = trim(expr.substr(1, open == expr.npos ? expr.npos : open - 1));
    auto function = std::find_if(std::begin(functions), std::end(functions),
                                 [name](const Function& candidate) { return candidate.name == name; });
    if (function == std::end(functions)) throw Unsupported("Unsupported modifier function: " + std::string(name));
    if (name == "split") scope.spend(1000);
    Args values;
    if (open == expr.npos) {
        if (scope.values.back()) {
            scope.spend(scope.values.back()->dump().size() / 16 + 1);
            values.push_back(*scope.values.back());
        }
    } else {
        if (!expr.ends_with(')')) throw Invalid("Unclosed modifier function");
        for (auto arg : arguments(expr.substr(open + 1, expr.size() - open - 2))) {
            if (values.size() == 256) throw LimitExceeded("Modifier argument limit exceeded");
            values.push_back(evaluate(arg, scope, true, depth + 1).value_or(nullptr));
        }
    }
    return function->apply(values);
}

Result evaluate(std::string_view expr, const Scope& scope, bool argument, unsigned depth) {
    if (depth > 32) throw LimitExceeded("Modifier expression nesting limit exceeded");
    scope.spend(expr.size() / 16 + 1);
    if (expr.starts_with('\\')) return Json(expr.substr(1));
    if (expr.starts_with('=')) return evaluate_function(expr, scope, depth);
    if (expr.starts_with('@') || expr.starts_with('^')) {
        const Json* node = expr.front() == '^' ? &scope.context : scope.values.back();
        auto path = expr.substr(1);
        if (expr.front() == '@' && path.starts_with('(')) {
            if (!path.ends_with(')')) throw Invalid("Unclosed modifier reference");
            path = path.substr(1, path.size() - 2);
            auto comma = path.find(',');
            if (comma != path.npos) {
                auto parent = read_number<std::size_t>(trim(path.substr(0, comma)));
                if (!parent || *parent >= scope.values.size()) return {};
                node = scope.values[scope.values.size() - 1 - *parent];
                path = trim(path.substr(comma + 1));
            }
        }
        auto found = lookup(node, path, scope);
        if (found) scope.spend(found->dump().size() / 16 + 1);
        return found ? Result(*found) : Result{};
    }
    if (!argument) return Json(expr);
    if (expr.size() >= 2 && expr.front() == '\'' && expr.back() == '\'') {
        std::string out;
        for (std::size_t i = 1; i + 1 < expr.size(); ++i) {
            if (expr[i] == '\\' && i + 2 < expr.size()) ++i;
            out += expr[i];
        }
        return Json(out);
    }
    auto parsed = Json::parse(expr, nullptr, false);
    return parsed.is_discarded() ? Json(expr) : std::move(parsed);
}

struct Rule {
    std::string_view name;
    ModifyMode mode;
    bool existing_only;
    bool array_index;
};

Rule rule_key(std::string_view key, ModifyMode mode) {
    if (key.empty()) throw Invalid("Empty modifier key");
    if (key.front() == '+' || key.front() == '~' || key.front() == '_') {
        mode = key.front() == '+' ? ModifyMode::overwrite :
               key.front() == '~' ? ModifyMode::defaults : ModifyMode::define;
        key.remove_prefix(1);
    }
    bool existing_only = key.ends_with('?');
    if (existing_only) key.remove_suffix(1);
    bool array_index = key.starts_with('[') && key.ends_with(']');
    if (array_index) key = key.substr(1, key.size() - 2);
    if (key.empty()) throw Invalid("Empty modifier key");
    if (key.find_first_of("@#$|&?[]\\") != key.npos || std::count(key.begin(), key.end(), '*') > 1)
        throw Unsupported("Unsupported modifier key pattern");
    if (array_index && key != "*") {
        auto index = read_number<std::size_t>(key);
        if (!index || *index > max_index) throw Invalid("Invalid modifier array index");
    }
    return {key, mode, existing_only, array_index};
}

bool matches(std::string_view pattern, std::string_view key) {
    auto star = pattern.find('*');
    if (star == pattern.npos) return pattern == key;
    auto left = pattern.substr(0, star), right = pattern.substr(star + 1);
    return key.size() >= left.size() + right.size() && key.starts_with(left) && key.ends_with(right);
}

Result candidate(const Json& rule, const Scope& scope) {
    if (rule.is_string()) return evaluate(rule.get_ref<const std::string&>(), scope);
    if (rule.is_array()) {
        for (const auto& choice : rule) {
            if (auto value = candidate(choice, scope)) return value;
        }
        return {};
    }
    return rule;
}

bool permits(ModifyMode mode, const Json* child) {
    return mode == ModifyMode::overwrite || !child ||
           (mode == ModifyMode::defaults && child->is_null());
}

void apply(Json& target, const Json& spec, ModifyMode mode, Scope& scope);

Result apply_child(Json* child, const Json& specification, const Rule& rule, Scope& scope) {
    if (!child && rule.existing_only) return {};
    if (specification.is_object()) {
        if (child && child->is_structured()) {
            scope.values.push_back(child);
            apply(*child, specification, rule.mode, scope);
            scope.values.pop_back();
            return {};
        }
        if (!permits(rule.mode, child)) return {};
        bool need_array = false;
        for (auto it = specification.begin(); it != specification.end(); ++it)
            need_array = need_array || rule_key(it.key(), rule.mode).array_index;
        auto created = need_array ? Json::array() : Json::object();
        // Attach a new parent before resolving child references.
        scope.values.push_back(&created);
        apply(created, specification, rule.mode, scope);
        scope.values.pop_back();
        return created;
    }
    if (!permits(rule.mode, child)) return {};
    scope.values.push_back(child);
    auto result = candidate(specification, scope);
    scope.values.pop_back();
    if (result) scope.spend(result->dump().size() / 16 + 1);
    return result;
}

void apply(Json& target, const Json& spec, ModifyMode mode, Scope& scope) {
    if (!spec.is_object()) throw Invalid("Modifier requires an object specification");
    if (!target.is_structured()) return;
    auto run = [&](std::string key, const Json& specification, const Rule& rule) {
        scope.spend();
        if (rule.array_index && !target.is_array()) return;
        scope.keys.push_back(key);
        if (target.is_object()) {
            auto it = target.find(key);
            auto value = apply_child(it == target.end() ? nullptr : &*it, specification, rule, scope);
            if (value) target[key] = std::move(*value);
        } else if (auto index = read_number<std::size_t>(key); index && *index <= max_index) {
            auto value = apply_child(*index < target.size() ? &target[*index] : nullptr, specification, rule, scope);
            if (value) {
                if (*index >= target.size()) scope.spend(*index + 1 - target.size());
                target[*index] = std::move(*value);
            }
        }
        scope.keys.pop_back();
    };

    for (auto it = spec.begin(); it != spec.end(); ++it) {
        auto rule = rule_key(it.key(), mode);
        if (rule.name.find('*') == rule.name.npos) run(std::string(rule.name), *it, rule);
    }
    std::vector<std::string> existing;
    if (target.is_object()) {
        for (auto it = target.begin(); it != target.end(); ++it) existing.push_back(it.key());
    } else {
        for (std::size_t i = 0; i < target.size(); ++i) existing.push_back(std::to_string(i));
    }
    for (const auto& key : existing) {
        scope.spend(spec.size() + 1);
        for (auto it = spec.begin(); it != spec.end(); ++it) {
            auto rule = rule_key(it.key(), mode);
            if (rule.name.find('*') != rule.name.npos && matches(rule.name, key)) {
                run(key, *it, rule);
                break;
            }
        }
    }
}

} // namespace

void modify(Json& input, const Json& specification, const Json& context,
            ModifyMode mode, std::size_t& remaining) {
    Scope scope{context, {&input}, {""}, remaining};
    apply(input, specification, mode, scope);
}

} // namespace jolt::detail
