// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace apolo::jolt {

class Unsupported : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Invalid : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class LimitExceeded : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Executes a Chainr specification and serializes the transformed JSON.
std::string transform(std::string_view input, std::string_view specification);

}
