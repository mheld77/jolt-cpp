// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <nlohmann/json.hpp>
#include <string_view>

namespace jolt::detail {

using Json = nlohmann::ordered_json;
enum class ModifyMode { overwrite, defaults, define };

void modify(Json& input, const Json& specification, const Json& context,
            ModifyMode mode, std::size_t& remaining);

}
