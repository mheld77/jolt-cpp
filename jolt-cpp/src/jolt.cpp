// SPDX-License-Identifier: Apache-2.0
// Native implementation of a subset of the JOLT Community Chainr contract.
// See NOTICE for the original project and the differences in this implementation.
#include "apolo/jolt.hpp"

#include <charconv>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace apolo::jolt {
namespace {

using Json = nlohmann::ordered_json;
using namespace std::string_view_literals;
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
    std::optional<std::string> capture;
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
    if (ref.group == 1 && matched.capture) return *matched.capture;
    throw Unsupported("Unsupported wildcard capture");
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
            auto close = path.find(']', i);
            if (close == path.npos) throw Invalid("Unclosed output index");
            auto index = path.substr(i, close - i);
            if (index.empty()) result.push_back({PathPart::append, {}});
            else if (index.starts_with('#')) {
                std::size_t position = 1;
                auto ref = read_reference(index, position);
                if (position != index.size() || ref.group != 0) throw Invalid("Invalid hash index");
                result.push_back({PathPart::index, {}, frame(walk, ref.parent).matches});
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

std::optional<std::string> wildcard(std::string_view pattern, std::string_view key) {
    const auto star = pattern.find('*');
    if (star == pattern.npos) return {};
    auto left = pattern.substr(0, star);
    auto right = pattern.substr(star + 1);
    if (right.find('*') != right.npos || pattern.find('|') != pattern.npos)
        throw Unsupported("Multiple wildcard captures or wildcard alternatives");
    if (key.size() < left.size() + right.size() || !key.starts_with(left) || !key.ends_with(right))
        return {};
    return std::string(key.substr(left.size(), key.size() - left.size() - right.size()));
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
            if (!special && key.find('*') != key.npos) wildcard(key, ""sv);
            validate_shift(*it);
        }
    } else if (spec.is_array()) {
        for (const auto& path : spec) {
            if (!path.is_string()) throw Invalid("Shift output paths must be strings");
            validate_shift(path);
        }
    } else if (!spec.is_null()) {
        if (!spec.is_string()) throw Invalid("Invalid shift output path");
        const auto& path = spec.get_ref<const std::string&>();
        if (path.find("[@") != path.npos)
            throw Unsupported("Unsupported array index expression");
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
            match.capture.reset();
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
                auto capture = wild ? wildcard(pattern, key) : std::nullopt;
                if (wild ? !capture.has_value() : !literal_or(pattern, key)) continue;
                walk.push_back({key, &value, std::move(capture)});
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

enum class Edit { defaults, remove, cardinality, overwrite, define };

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
        else if ((mode == Edit::overwrite || mode == Edit::define) && it->is_array())
            throw Unsupported("Modifier candidate lists");
        else if ((mode == Edit::overwrite || mode == Edit::define) && it->is_string()) {
            const auto& value = it->get_ref<const std::string&>();
            if (value.starts_with('=') || value.starts_with('@') || value.starts_with('^'))
                throw Unsupported("Modifier function or reference");
        }
    }
}

void edit(Json& target, const Json& spec, Edit mode, Budget& budget);

void edit_child(Json& child, const Json& rule, Edit mode, Budget& budget) {
    if (rule.is_object()) {
        if (child.is_null() && (mode == Edit::defaults || mode == Edit::overwrite || mode == Edit::define))
            child = Json::object();
        edit(child, rule, mode, budget);
    } else if (mode == Edit::cardinality) {
        if (rule == "ONE" && child.is_array()) child = child.empty() ? Json(nullptr) : Json(child[0]);
        if (rule == "MANY" && !child.is_array())
            child = child.is_null() ? Json::array() : Json::array({child});
    } else if (mode == Edit::overwrite || (mode == Edit::defaults && child.is_null())) {
        budget.spend(rule.is_structured() ? rule.dump().size() / 16 + 1 : 1);
        child = rule;
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
            } else if (mode == Edit::defaults || mode == Edit::overwrite || mode == Edit::define) {
                if (mode == Edit::define && !rule.is_object()) target[key] = rule;
                else edit_child(target[key], rule, mode, budget);
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
    try {
        auto source = parse(input);
        const auto steps = parse(specification);
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
                Walk walk{{"", &source, std::nullopt}};
                Output output{nullptr, {}, budget};
                shift(source, spec, walk, output, budget);
                source = std::move(output.value);
            } else {
                Edit mode;
                if (operation == "default" || operation == "modify-default-beta") mode = Edit::defaults;
                else if (operation == "remove") mode = Edit::remove;
                else if (operation == "cardinality") mode = Edit::cardinality;
                else if (operation == "modify-overwrite-beta") mode = Edit::overwrite;
                else if (operation == "modify-define-beta") mode = Edit::define;
                else throw Unsupported("Operation is not implemented by this native library");
                validate_edit(spec, mode);
                if (operation == "modify-default-beta") validate_edit(spec, Edit::overwrite);
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

} // namespace apolo::jolt
