# Jolt C++

A native C++20 library for Jolt-style JSON-to-JSON transformations. Pass a JSON document and a Chainr specification to `jolt::transform` to get the transformed JSON. The library runs without a JVM.

## Build and test

Requires CMake, a C++20 compiler, and nlohmann/json 3.11 or newer.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$PWD/build/install"
```

Link the CMake target `jolt_cpp` or the installed static library `libjolt_cpp.a` and include [jolt/jolt.hpp](include/jolt/jolt.hpp).

```cpp
#include <jolt/jolt.hpp>

auto result = jolt::transform(
    R"({"title":"Coffee"})",
    R"([{"operation":"shift","spec":{"title":"product.name"}}])");
// {"product":{"name":"Coffee"}}
```

## Supported syntax

| Operation | Coverage |
| --- | --- |
| `shift` | Literal keys and alternatives, a single wildcard per pattern, nested output paths, `&n`, `&(n,1)`, explicit indexes, `[]`, `[#n]`, `@`, `@(n,path)`, `$n`, and `#value`. |
| `default`, `remove`, `cardinality` | Literal keys, explicit indexes on existing arrays and simple wildcards. `remove` supports literal alternatives. Cardinality supports `ONE` and `MANY`. |
| `modify-overwrite-beta`, `modify-default-beta`, `modify-define-beta` | Literal values and nested keys. |
| `sort` | Recursive sorting of object keys. |

Compatibility with the Java DSL is partial. Modifier functions, transposed array indexes, multiple-wildcard patterns, custom Java classes, and extra operations such as `enrich` are outside the current coverage. The engine limits input and output size, nesting, array indexes, and work per transformation. See the header for public exception types.

## Reference and licensing

[Jolt Community](https://github.com/jolt-community/jolt-community), a fork of Bazaarvoice Jolt, is the reference for the transformation contract. The C++ implementation and its reference revision are documented in [NOTICE](NOTICE).

The library is distributed under the [Apache License 2.0](LICENSE). Its nlohmann/json dependency is distributed under the [MIT license](LICENSE.nlohmann-json).
