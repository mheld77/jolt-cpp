// SPDX-License-Identifier: Apache-2.0
#include "apolo/jolt.hpp"
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

using apolo::jolt::transform;
using Json = nlohmann::json;

void expect(const char* input, const char* spec, const char* expected) {
    auto actual = transform(input, spec);
    if (Json::parse(actual) != Json::parse(expected))
        throw std::runtime_error(std::string("Unexpected result: ") + actual + ", expected: " + expected);
}

int main() {
    expect(R"({"payload":{"products":[{"id":1,"title":"Maçã"},{"id":2,"title":"Café"}],"total":2}})",
           R"([{"operation":"shift","spec":{"payload":{"products":{"*":{"id":"payloadOut.produtos[&1].id","title":"payloadOut.produtos[&1].nome"}},"total":"payloadOut.total"}}}])",
           R"({"payloadOut":{"produtos":[{"id":1,"nome":"Maçã"},{"id":2,"nome":"Café"}],"total":2}})");
    expect(R"({"one":{"a":1},"two":{"b":2}})", R"([{"operation":"shift","spec":{"*":"items[]"}}])",
           R"({"items":[{"a":1},{"b":2}]})");
    expect(R"({"first_name":"Ana","last_name":"Silva"})",
           R"jolt([{"operation":"shift","spec":{"*_name":"names.&(0,1)"}}])jolt",
           R"({"names":{"first":"Ana","last":"Silva"}})");
    expect(R"({"key":1})", R"([{"operation":"shift","spec":{"missing":"out"}}])", "null");
    expect(R"({"key":1})", R"([{"operation":"shift","spec":{"@":""}}])", R"({"key":1})");
    expect(R"({"metrics":{"cpu":2,"mem":4}})",
           R"([{"operation":"shift","spec":{"metrics":{"*":{"$":"items[#2].name","@":"items[#2].value"}}}}])",
           R"({"items":[{"name":"cpu","value":2},{"name":"mem","value":4}]})");
    expect(R"({"a":1,"b":2,"c":3})", R"([{"operation":"remove","spec":{"a|b":""}}])", R"({"c":3})");
    expect(R"({"groups":{"first":["red","blue"],"second":["red"]}})",
           R"([{"operation":"shift","spec":{"groups":{"*":{"*":{"*":{"$2":"&1[]"}}}}}}])",
           R"({"red":["first","second"],"blue":["first"]})");
    expect(R"({"a":null,"items":[1,2,3,4],"removed":1})",
           R"([{"operation":"default","spec":{"a":"x","tag":"new"}},
                {"operation":"remove","spec":{"removed":"","items":{"0":"","2":""}}},
                {"operation":"cardinality","spec":{"a":"MANY","items":"ONE"}},
                {"operation":"modify-define-beta","spec":{"local":true}},
                {"operation":"modify-overwrite-beta","spec":{"tag":"changed"}},
                {"operation":"modify-default-beta","spec":{"extra":5}},
                {"operation":"sort"}])",
           R"({"a":["x"],"items":2,"local":true,"tag":"changed","extra":5})");
    expect(R"({"a":1})", R"([{"operation":"shift","spec":{"a":"[0]"}}])", "[1]");

    try {
        transform("{}", R"([{"operation":"modify-overwrite-beta","spec":{"x":"=toUpper"}}])");
        throw std::runtime_error("Expected Unsupported");
    } catch (const apolo::jolt::Unsupported&) {}
    try {
        transform("{}", "[");
        throw std::runtime_error("Expected Invalid");
    } catch (const apolo::jolt::Invalid&) {}
    try {
        transform(std::string(129, '[') + std::string(129, ']'), R"([{"operation":"sort"}])");
        throw std::runtime_error("Expected LimitExceeded");
    } catch (const apolo::jolt::LimitExceeded&) {}

    std::cout << "Jolt C++ tests passed\n";
}
