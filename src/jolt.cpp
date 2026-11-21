// SPDX-License-Identifier: Apache-2.0
// Native implementation of a subset of the JOLT Community Chainr contract.
// See NOTICE for the original project and the differences in this implementation.
#include "jolt/jolt.hpp"
#include "modifier.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace jolt {
namespace {

using Json = nlohmann::ordered_json;
constexpr std::size_t max_bytes = 4 * 1024 * 1024;
constexpr std::size_t max_depth = 128;
constexpr std::size_t max_index = 100000;
constexpr std::size_t max_work = 250000;
constexpr std::size_t max_path_bytes = 8192;

struct Budget {
    std::size_t remaining = max_work;

    void spend(std::size_t amount = 1) {
        if (amount > remaining) throw LimitExceeded("Transformation work limit exceeded");
        remaining -= amount;
    }
};

std::size_t number(std::string_view text) {
    std::size_t value{};
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value > max_index)
        throw Invalid("Invalid index or reference");
    return value;
}

Json parse(std::string_view text) {
    if (text.size() > max_bytes) throw LimitExceeded("JSON input limit exceeded");
    return Json::parse(text, [](int depth, Json::parse_event_t, Json&) {
        if (depth >= static_cast<int>(max_depth)) throw LimitExceeded("JSON nesting limit exceeded");
        return true;
    });
}

template<class Function>
void each(const Json& value, Function action) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) action(it.key(), *it);
    } else if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) action(std::to_string(i), value[i]);
    }
}

struct Frame {
    std::string key;
    const Json* value;
    std::vector<std::string> captures;
    std::size_t matches{};
};
using Walk = std::vector<Frame>;

const Frame& frame(const Walk& walk, std::size_t parent) {
    if (parent >= walk.size()) throw Invalid("Reference outside input path");
    return walk[walk.size() - 1 - parent];
}

struct Reference {
    std::size_t parent{};
    std::size_t group{};
};

Reference read_reference(std::string_view expr, std::size_t& position) {
    Reference result;
    if (position < expr.size() && expr[position] == '(') {
        auto close = expr.find(')', ++position);
        if (close == expr.npos) throw Invalid("Unclosed path reference");
        const auto values = expr.substr(position, close - position);
        auto comma = values.find(',');
        result.parent = number(values.substr(0, comma));
        if (comma != values.npos) result.group = number(values.substr(comma + 1));
        position = close + 1;
    } else {
        auto start = position;
        while (position < expr.size() && expr[position] >= '0' && expr[position] <= '9') ++position;
        if (position != start) result.parent = number(expr.substr(start, position - start));
    }
    return result;
}

const std::string& referenced_key(const Walk& walk, const Reference& ref) {
    const auto& matched = frame(walk, ref.parent);
    if (ref.group == 0) return matched.key;
    if (ref.group <= matched.captures.size()) return matched.captures[ref.group - 1];
    throw Invalid("Wildcard capture does not exist");
}

std::string text_value(const Json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number() || value.is_boolean()) return value.dump();
    throw Invalid("Path reference must resolve to a scalar key");
}

const Json* lookup(const Json& root, std::string_view path) {
    const Json* node = &root;
    while (!path.empty()) {
        auto stop = path.find_first_of(".[");
        auto key = path.substr(0, stop);
        if (!key.empty()) {
            if (!node->is_object()) return nullptr;
            auto found = node->find(std::string(key));
            if (found == node->end()) return nullptr;
            node = &*found;
        }
        if (stop == std::string_view::npos) break;
        path.remove_prefix(stop);
        if (path.front() == '[') {
            auto close = path.find(']');
            if (close == path.npos) throw Invalid("Unclosed lookup index");
            auto index = number(path.substr(1, close - 1));
            if (!node->is_array() || index >= node->size()) return nullptr;
            node = &(*node)[index];
            path.remove_prefix(close + 1);
        }
        if (path.starts_with('.')) path.remove_prefix(1);
    }
    return node;
}

const Json* transpose(const Walk& walk, std::string_view expression) {
    auto comma = expression.find(',');
    if (comma == expression.npos) return lookup(*walk.back().value, expression);
    const auto& parent = frame(walk, number(expression.substr(0, comma)));
    return lookup(*parent.value, expression.substr(comma + 1));
}

struct PathPart {
    enum Kind { field, index, append } kind;
    std::string key;
    std::size_t position{};
};

std::vector<PathPart> output_path(std::string_view path, const Walk& walk) {
    std::vector<PathPart> result;
    std::string field;
    auto flush = [&] {
        if (!field.empty()) {
            result.push_back({PathPart::field, field});
            field.clear();
        }
    };
    for (std::size_t i = 0; i < path.size();) {
        char ch = path[i++];
        if (ch == '\\') {
            if (i == path.size()) throw Invalid("Incomplete path escape");
            field += path[i++];
        } else if (ch == '.') {
            flush();
        } else if (ch == '[') {
            flush();
            auto close = i;
            std::size_t parentheses = 0;
            for (; close < path.size(); ++close) {
                if (path[close] == '(') ++parentheses;
                else if (path[close] == ')' && parentheses) --parentheses;
                else if (path[close] == ']' && !parentheses) break;
            }
            if (close == path.size()) throw Invalid("Unclosed output index");
            auto index = path.substr(i, close - i);
            if (index.empty()) result.push_back({PathPart::append, {}});
            else if (index.starts_with('#')) {
                std::size_t position = 1;
                auto ref = read_reference(index, position);
                if (position != index.size() || ref.group != 0) throw Invalid("Invalid hash index");
                result.push_back({PathPart::index, {}, frame(walk, ref.parent).matches});
            } else if (index.starts_with("@(") && index.ends_with(')')) {
                auto* data = transpose(walk, index.substr(2, index.size() - 3));
                if (!data || (!data->is_string() && !data->is_number_integer())) return {};
                auto integer = text_value(*data);
                std::size_t value{};
                auto [end, error] = std::from_chars(integer.data(), integer.data() + integer.size(), value);
                if (error != std::errc{} || end != integer.data() + integer.size()) return {};
                if (value > max_index) throw LimitExceeded("Output array limit exceeded");
                result.push_back({PathPart::index, {}, value});
            } else {
                auto reference = output_path(index, walk);
                if (reference.size() != 1 || reference.front().kind != PathPart::field)
                    throw Unsupported("Unsupported array index expression");
                result.push_back({PathPart::index, {}, number(reference.front().key)});
            }
            i = close + 1;
        } else if (ch == '&') {
            field += referenced_key(walk, read_reference(path, i));
        } else if (ch == '@' && i < path.size() && path[i] == '(') {
            auto close = path.find(')', ++i);
            if (close == path.npos) throw Invalid("Unclosed input reference");
            auto* value = transpose(walk, path.substr(i, close - i));
            if (!value) return {};
            field += text_value(*value);
            i = close + 1;
        } else if (ch == '#' || ch == '@' || ch == ']' || ch == '$') {
            throw Unsupported("Unsupported output path operator");
        } else {
            field += ch;
        }
        if (result.size() > max_depth || field.size() > max_path_bytes)
            throw LimitExceeded("Output path limit exceeded");
    }
    flush();
    return result;
}

struct Output {
    Json value;
    std::set<std::string> assigned;
    Budget& budget;

    void write(std::string_view path, const Walk& walk, const Json& input) {
        auto parts = output_path(path, walk);
        if (!path.empty() && parts.empty()) return;
        auto* node = &value;
        std::string address;
        for (auto& part : parts) {
            if (part.kind == PathPart::field) {
                if (node->is_null()) *node = Json::object();
                if (!node->is_object()) throw Invalid("Object and array paths conflict");
                node = &(*node)[part.key];
                address += "/k" + std::to_string(part.key.size()) + ":" + part.key;
            } else {
                if (node->is_null()) *node = Json::array();
                if (!node->is_array()) throw Invalid("Object and array paths conflict");
                auto i = part.kind == PathPart::append ? node->size() : part.position;
                if (i > max_index) throw LimitExceeded("Output array limit exceeded");
                if (i >= node->size()) budget.spend(i - node->size() + 1);
                node = &(*node)[i];
                address += "/i" + std::to_string(i);
            }
        }
        budget.spend(input.dump().size() / 16 + address.size() / 16 + 1);
        if (assigned.insert(address).second) {
            *node = input;
        } else if (node->is_array()) {
            node->push_back(input);
        } else {
            *node = Json::array({*node, input});
        }
    }
};

bool literal_or(std::string_view pattern, std::string_view key) {
    while (true) {
        const auto separator = pattern.find('|');
        if (pattern.substr(0, separator) == key) return true;
        if (separator == pattern.npos) return false;
        pattern.remove_prefix(separator + 1);
    }
}

bool special_key(std::string_view key) {
    return key == "@" || key.starts_with('$') || key.starts_with('#') ||
           (key.starts_with("@(") && key.ends_with(')'));
}

bool capture_pattern(const std::vector<std::string_view>& literals, unsigned index,
                     std::string_view rest, std::vector<std::string>& captures, Budget& budget) {
    budget.spend();
    auto literal = literals[index];
    if (index + 1 == literals.size()) {
        if (!rest.ends_with(literal) || rest.size() <= literal.size()) return false;
        captures.emplace_back(rest.substr(0, rest.size() - literal.size()));
        return true;
    }
    for (auto next = rest.find(literal, 1); next != rest.npos;
         next = rest.find(literal, next + 1)) {
        budget.spend();
        captures.emplace_back(rest.substr(0, next));
        if (capture_pattern(literals, index + 1, rest.substr(next + literal.size()), captures, budget))
            return true;
        captures.pop_back();
        if (next == rest.size()) break;
    }
    return false;
}

std::optional<std::vector<std::string>> wildcard(std::string_view pattern, std::string_view key, Budget& budget) {
    while (true) {
        const auto separator = pattern.find('|');
        auto alt = pattern.substr(0, separator);
        const auto star = alt.find('*');
        if (star == alt.npos) {
            if (alt == key) return std::vector<std::string>{};
        } else if (key.starts_with(alt.substr(0, star))) {
            std::vector<std::string_view> literals;
            auto remainder = alt.substr(star + 1);
            for (;;) {
                auto next = remainder.find('*');
                literals.push_back(remainder.substr(0, next));
                if (next == remainder.npos) break;
                remainder.remove_prefix(next + 1);
            }
            if (literals.size() > 16) throw LimitExceeded("Wildcard capture limit exceeded");
            auto rest = key.substr(star);
            std::vector<std::string> captures;
            if (alt == "*" && rest.empty()) return std::vector<std::string>{""};
            if (capture_pattern(literals, 0, rest, captures, budget)) return captures;
        }
        if (separator == pattern.npos) return {};
        pattern.remove_prefix(separator + 1);
    }
}

void validate_shift(const Json& spec) {
    if (spec.is_object()) {
        for (auto it = spec.begin(); it != spec.end(); ++it) {
            const std::string_view key = it.key();
            bool special = special_key(key);
            if ((key == "@" || key.starts_with('$') || key.starts_with('#')) && it->is_object())
                throw Invalid("Special shift key requires an output leaf");
            if (key.starts_with('$')) {
                std::size_t position = 1;
                read_reference(key, position);
                if (position != key.size()) throw Invalid("Invalid dollar reference");
            }
            if (!special && key.find_first_of("@$#&[]()?+!\\") != key.npos)
                throw Unsupported("Unsupported shift input operator");
            if (std::count(key.begin(), key.end(), '*') > 16)
                throw LimitExceeded("Wildcard capture limit exceeded");
            validate_shift(*it);
        }
    } else if (spec.is_array()) {
        for (const auto& path : spec) {
            if (!path.is_string()) throw Invalid("Shift output paths must be strings");
            validate_shift(path);
        }
    } else if (!spec.is_null()) {
        if (!spec.is_string()) throw Invalid("Invalid shift output path");
    }
}

void shift(const Json& source, const Json& spec, Walk& walk, Output& output, Budget& budget) {
    budget.spend();
    if (spec.is_string()) {
        output.write(spec.get_ref<const std::string&>(), walk, source);
        return;
    }
    if (spec.is_array()) {
        for (const auto& path : spec) output.write(path.get_ref<const std::string&>(), walk, source);
        return;
    }
    if (spec.is_null()) return;
    for (auto it = spec.begin(); it != spec.end(); ++it) {
        const std::string_view key = it.key();
        if (!special_key(key)) continue;
        Frame match = walk.back();
        const Json* value = &source;
        Json literal;
        if (key.starts_with('$') || key.starts_with('#')) {
            std::size_t position = 1;
            match.key = key.starts_with('$') ? referenced_key(walk, read_reference(key, position))
                                             : std::string(key.substr(1));
            match.captures.clear();
            match.matches = 0;
            literal = match.key;
            value = &literal;
        } else if (key.starts_with("@(")) {
            value = transpose(walk, key.substr(2, key.size() - 3));
            if (!value) continue;
        }
        match.value = value;
        walk.push_back(std::move(match));
        shift(*value, *it, walk, output, budget);
        walk.pop_back();
    }
    auto match_child = [&](const auto& key, const Json& value) {
        budget.spend(spec.size());
        for (bool wild : {false, true}) {
            for (auto it = spec.begin(); it != spec.end(); ++it) {
                std::string_view pattern = it.key();
                if (special_key(pattern)) continue;
                if ((pattern.find('*') != pattern.npos) != wild) continue;
                auto capture = wild ? wildcard(pattern, key, budget) : std::nullopt;
                if (wild ? !capture.has_value() : !literal_or(pattern, key)) continue;
                walk.push_back({key, &value, capture.value_or(std::vector<std::string>{})});
                shift(value, *it, walk, output, budget);
                walk.pop_back();
                ++walk.back().matches;
                return;
            }
        }
    };
    if (source.is_structured()) each(source, match_child);
    else if (!source.is_null()) match_child(text_value(source), Json(nullptr));
}

void defaults(Json& target, const Json& spec, Budget& budget);

void set_default(Json& target, const Json& spec, bool array, Budget& budget) {
    if (!spec.is_object()) {
        if (target.is_null()) {
            budget.spend(spec.dump().size() / 16 + 1);
            target = spec;
        }
        return;
    }
    if (target.is_null()) target = array ? Json::array() : Json::object();
    if ((array && target.is_array()) || (!array && target.is_object()))
        defaults(target, spec, budget);
}

void defaults(Json& target, const Json& spec, Budget& budget) {
    if (!spec.is_object()) throw Invalid("Default requires an object specification");
    if (!target.is_structured()) return;

    struct DefaultRule {
        std::string_view name;
        const Json* value;
        bool array;
        std::size_t alternatives;
    };
    std::vector<DefaultRule> rules;
    rules.reserve(spec.size());
    for (auto it = spec.begin(); it != spec.end(); ++it) {
        std::string_view name = it.key();
        const bool array = name.ends_with("[]");
        if (array) name.remove_suffix(2);
        if (name.empty() || (name != "*" && name.find_first_of("*[]") != name.npos))
            throw Unsupported("Unsupported default key");
        if (array && !it->is_object()) throw Invalid("Array default requires an object specification");
        const auto alternatives = static_cast<std::size_t>(std::count(name.begin(), name.end(), '|'));
        rules.push_back({name, &*it, array, alternatives});
    }
    std::sort(rules.begin(), rules.end(), [](const DefaultRule& a, const DefaultRule& b) {
        if ((a.name == "*") != (b.name == "*")) return b.name == "*";
        if (a.alternatives != b.alternatives) return a.alternatives < b.alternatives;
        return a.name < b.name;
    });
    for (const auto& rule : rules) {
        budget.spend();
        if (rule.name == "*" || rule.alternatives) {
            if (target.is_array()) {
                for (std::size_t i = 0; i < target.size(); ++i) {
                    budget.spend();
                    if (rule.name == "*" || literal_or(rule.name, std::to_string(i)))
                        set_default(target[i], *rule.value, rule.array, budget);
                }
            } else {
                for (auto it = target.begin(); it != target.end(); ++it) {
                    budget.spend();
                    if (rule.name == "*" || literal_or(rule.name, it.key()))
                        set_default(*it, *rule.value, rule.array, budget);
                }
            }
        } else if (target.is_array()) {
            auto index = number(rule.name);
            if (index >= target.size()) budget.spend(index + 1 - target.size());
            set_default(target[index], *rule.value, rule.array, budget);
        } else {
            set_default(target[std::string(rule.name)], *rule.value, rule.array, budget);
        }
    }
}

enum class Edit { remove, cardinality };

void validate_edit(const Json& spec, Edit mode) {
    if (!spec.is_object()) throw Invalid("Operation requires an object specification");
    for (auto it = spec.begin(); it != spec.end(); ++it) {
        std::string_view key = it.key();
        if (key != "*" && key.find_first_of("*@#$&[]()?+!\\") != key.npos)
            throw Unsupported("Unsupported key operator in tree operation");
        if (mode != Edit::remove && key.find('|') != key.npos)
            throw Unsupported("Key alternatives in this tree operation");
        if (it->is_object()) validate_edit(*it, mode);
        else if (mode == Edit::cardinality && *it != "ONE" && *it != "MANY")
            throw Invalid("Cardinality must be ONE or MANY");
        else if (mode == Edit::remove && !it->is_string())
            throw Invalid("Remove leaf must be a string");
    }
}

void edit(Json& target, const Json& spec, Edit mode, Budget& budget);

void edit_child(Json& child, const Json& rule, Edit mode, Budget& budget) {
    if (rule.is_object()) {
        edit(child, rule, mode, budget);
    } else if (mode == Edit::cardinality) {
        if (rule == "ONE" && child.is_array()) child = child.empty() ? Json(nullptr) : Json(child[0]);
        if (rule == "MANY" && !child.is_array())
            child = child.is_null() ? Json::array() : Json::array({child});
    }
}

void edit(Json& target, const Json& spec, Edit mode, Budget& budget) {
    budget.spend(spec.size());
    if (target.is_array()) {
        auto star = spec.find("*");
        for (auto i = target.size(); i > 0; --i) {
            budget.spend();
            auto key = std::to_string(i - 1);
            auto exact = spec.find(key);
            if (exact == spec.end() && mode == Edit::remove) {
                for (auto it = spec.begin(); it != spec.end(); ++it) {
                    if (literal_or(it.key(), key)) {
                        exact = it;
                        break;
                    }
                }
            }
            auto rule = exact != spec.end() ? exact : star;
            if (rule == spec.end()) continue;
            if (mode == Edit::remove && !rule->is_object()) target.erase(i - 1);
            else edit_child(target[i - 1], *rule, mode, budget);
        }
        return;
    }
    auto apply = [&](const std::string& key, const Json& rule) {
        budget.spend();
        if (target.is_object()) {
            auto child = target.find(key);
            if (mode == Edit::remove && !rule.is_object()) {
                if (child != target.end()) target.erase(child);
            } else if (child != target.end()) {
                edit_child(*child, rule, mode, budget);
            }
        }
    };
    for (auto it = spec.begin(); it != spec.end(); ++it) {
        std::string_view key = it.key();
        if (key == "*") continue;
        for (;;) {
            auto separator = key.find('|');
            apply(std::string(key.substr(0, separator)), *it);
            if (separator == key.npos) break;
            key.remove_prefix(separator + 1);
        }
    }
    if (auto star = spec.find("*"); star != spec.end()) {
        if (target.is_object()) {
            std::vector<std::string> keys;
            for (auto it = target.begin(); it != target.end(); ++it)
                if (!spec.contains(it.key())) keys.push_back(it.key());
            for (const auto& key : keys) apply(key, *star);
        }
    }
}

void check_depth(const Json& value, std::size_t depth = 0) {
    if (depth > max_depth) throw LimitExceeded("Output nesting limit exceeded");
    if (value.is_structured())
        for (const auto& child : value) check_depth(child, depth + 1);
}

} // namespace

std::string transform(std::string_view input, std::string_view specification) {
    return transform(input, specification, "{}");
}

std::string transform(std::string_view input, std::string_view specification, std::string_view context) {
    try {
        auto source = parse(input);
        const auto steps = parse(specification);
        const auto context_data = parse(context);
        if (!context_data.is_object()) throw Invalid("Chainr context must be an object");
        if (!steps.is_array() || steps.empty() || steps.size() > 64)
            throw Invalid("Expected a nonempty Chainr array with at most 64 operations");
        Budget budget;
        for (const auto& step : steps) {
            if (!step.is_object() || !step.contains("operation") || !step["operation"].is_string())
                throw Invalid("Operation is missing");
            const auto operation = step["operation"].get<std::string>();
            if (operation == "sort") {
                if (step.contains("spec")) throw Invalid("Sort does not take a specification");
                source = Json::parse(nlohmann::json::parse(source.dump()).dump());
                continue;
            }
            if (!step.contains("spec")) throw Invalid("Operation requires spec");
            const auto& spec = step["spec"];
            if (operation == "shift") {
                if (!spec.is_object()) throw Invalid("Shift requires an object specification");
                validate_shift(spec);
                Walk walk{{"", &source, {}}};
                Output output{nullptr, {}, budget};
                shift(source, spec, walk, output, budget);
                source = std::move(output.value);
            } else if (operation.starts_with("modify-")) {
                using detail::ModifyMode;
                ModifyMode mode;
                if (operation == "modify-overwrite-beta") mode = ModifyMode::overwrite;
                else if (operation == "modify-default-beta") mode = ModifyMode::defaults;
                else if (operation == "modify-define-beta") mode = ModifyMode::define;
                else throw Unsupported("Operation is not implemented by this native library");
                detail::modify(source, spec, context_data, mode, budget.remaining);
            } else if (operation == "default") {
                defaults(source, spec, budget);
            } else {
                Edit mode;
                if (operation == "remove") mode = Edit::remove;
                else if (operation == "cardinality") mode = Edit::cardinality;
                else throw Unsupported("Operation is not implemented by this native library");
                validate_edit(spec, mode);
                edit(source, spec, mode, budget);
            }
            check_depth(source);
            if (source.dump().size() > max_bytes) throw LimitExceeded("Output size limit exceeded");
        }
        return source.dump();
    } catch (const Json::exception& error) {
        throw Invalid(error.what());
    }
}

} // namespace jolt
