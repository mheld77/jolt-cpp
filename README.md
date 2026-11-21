# Jolt C++

A native C++20 library for Jolt-style JSON-to-JSON transformations. Pass a JSON document and a Chainr specification to `jolt::transform` to get the transformed JSON. The library runs without a JVM.

## 1. Install the build dependencies

The library requires CMake 3.18 or newer, a C++20 compiler, nlohmann/json 3.11 or newer, and ICU. On Debian 12 or Ubuntu 24.04, install the packages below.

```bash
sudo apt-get update
sudo apt-get install -y git build-essential cmake nlohmann-json3-dev libicu-dev
```

On other systems, install equivalent packages and make sure CMake can find the `nlohmann_json` and ICU development packages.

## 2. Clone and build

```bash
git clone --branch cpp --single-branch https://github.com/mheld77/jolt-cpp.git
cd jolt-cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 2
```

The build produces the static library `build/libjolt_cpp.a` and the test executable `build/jolt_cpp_tests`.

## 3. Run the test suite

```bash
ctest --test-dir build --output-on-failure
```

CTest should report `100% tests passed`. You can also run the test executable directly.

```bash
./build/jolt_cpp_tests
```

It prints `Jolt C++ tests passed` on success. The [test source](tests/transform.cpp) contains examples for nested paths, arrays, Unicode, chained operations, and error handling.

## 4. Compile a runnable example

From the repository root, create a small command-line program that reads an input file and a Chainr specification. It prints the transformed JSON to standard output and exits with an error status if the transformation fails.

```bash
cat > build/jolt-example.cpp <<'CPP'
#include <jolt/jolt.hpp>

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

std::string read_file(const char* path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error(std::string("Cannot open ") + path);
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: jolt-example INPUT.json SPEC.json\n";
        return 2;
    }

    try {
        std::cout << jolt::transform(read_file(argv[1]), read_file(argv[2])) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
CPP

c++ -std=c++20 -Iinclude build/jolt-example.cpp build/libjolt_cpp.a -licui18n -licuuc -licudata -o build/jolt-example
```

## 5. Run the examples and check the output

### Rename a field and write it to a nested path

```bash
cat > build/input.json <<'JSON'
{"title":"Coffee"}
JSON
cat > build/spec.json <<'JSON'
[
  {"operation":"shift","spec":{"title":"product.name"}}
]
JSON

./build/jolt-example build/input.json build/spec.json
```

**Expected output**

```json
{"product":{"name":"Coffee"}}
```

### Transform each item in an array

```bash
cat > build/input.json <<'JSON'
{"products":[{"id":1,"title":"Coffee","stock":6},{"id":2,"title":"Tea","stock":3}]}
JSON
cat > build/spec.json <<'JSON'
[
  {
    "operation": "shift",
    "spec": {
      "products": {
        "*": {
          "id": "items[&1].id",
          "title": "items[&1].name"
        }
      }
    }
  }
]
JSON

./build/jolt-example build/input.json build/spec.json
```

**Expected output**

```json
{"items":[{"id":1,"name":"Coffee"},{"id":2,"name":"Tea"}]}
```

### Chain multiple operations

```bash
cat > build/input.json <<'JSON'
{"title":"Coffee","internalId":42}
JSON
cat > build/spec.json <<'JSON'
[
  {"operation":"default","spec":{"status":"active"}},
  {"operation":"remove","spec":{"internalId":""}},
  {"operation":"sort"}
]
JSON

./build/jolt-example build/input.json build/spec.json
```

**Expected output**

```json
{"status":"active","title":"Coffee"}
```

### Compute fields with a modifier

```bash
cat > build/input.json <<'JSON'
{"title":"Café","price":"15"}
JSON
cat > build/spec.json <<'JSON'
[
  {
    "operation":"modify-overwrite-beta",
    "spec":{"name":"=toUpper(@(1,title))","price":"=toInteger"}
  }
]
JSON

./build/jolt-example build/input.json build/spec.json
```

**Expected output**

```json
{"title":"Café","price":15,"name":"CAFÉ"}
```

### Check an unsupported specification

Run this after any of the successful examples above. The example program should print an error and exit with a nonzero status.

```bash
cat > build/unsupported.json <<'JSON'
[
  {"operation":"modify-overwrite-beta","spec":{"title":"=dateAdd"}}
]
JSON

if ./build/jolt-example build/input.json build/unsupported.json; then
    printf '%s\n' 'FAIL: an unsupported specification was accepted' >&2
    exit 1
else
    printf '%s\n' 'PASS: the unsupported specification was rejected'
fi
```

The executable prints `Unsupported modifier function: dateAdd` to standard error. The shell check then prints `PASS: the unsupported specification was rejected`.

## Install or link the library

```bash
cmake --install build --prefix "$PWD/build/install"
```

The install directory contains the static library, [public header](include/jolt/jolt.hpp), and license notices. If you include the source as a CMake subdirectory, link against the `jolt_cpp` target.

## Supported syntax

| Operation | Coverage |
| --- | --- |
| `shift` | Literal keys and alternatives, multiple wildcard captures, nested output paths, `&n`, `&(n,1)`, explicit and transposed indexes, `[]`, `[#n]`, `@`, `@(n,path)`, `$n`, and `#value`. |
| `default` | Literal keys, alternatives, wildcards, and array defaults with `field[]`. |
| `remove`, `cardinality` | Literal keys, explicit indexes on existing arrays, and simple wildcards. `remove` supports literal alternatives. Cardinality supports `ONE` and `MANY`. |
| `modify-overwrite-beta`, `modify-default-beta`, `modify-define-beta` | Literal values, nested keys, array indexes, wildcards, relative references, fallback expressions, per-key mode overrides, and built-in string, numeric, type conversion, and list functions. |
| `sort` | Recursive sorting of object keys. |

Pass a JSON context as the third argument to `jolt::transform` to resolve modifier context references such as `^(1,key)`. The engine limits input and output size, nesting, array indexes, and work per transformation. See the [public header](include/jolt/jolt.hpp) for exception types.

## Java compatibility and migration

Compatibility with the Java DSL is partial. The table lists the known gaps most relevant to migrating existing specifications.

| Area | Current gaps |
| --- | --- |
| `shift` | Dynamic input keys using `&` and escaped input keys are unsupported. Overlapping wildcard rules can resolve in a different order. |
| `modify-*-beta` | Some advanced key patterns and Community-specific functions such as `dateAdd` are unsupported. Type coercion, nulls, and missing references need broader comparison across functions. |
| `remove`, `cardinality` | Partial wildcards such as `prefix*` are unsupported. `cardinality` also rejects key alternatives such as `a&#124;b`. |
| Extension operations | `enrich` and custom Java transform classes require separate native implementations. |

CTest covers the native engine. A separate comparison against Jolt Java 0.1.1 matched 18 Jolt Community fixtures and six additional modifier and advanced shift cases. The local test commands above run without a JVM.

Before switching an existing application, replay its saved specifications with representative input in both engines. Compare parsed JSON output, including array order, and include inputs with missing and null fields. Keep the Java engine available for mappings whose results differ or use unsupported syntax.

## Reference and licensing

[Jolt Community](https://github.com/jolt-community/jolt-community), a fork of Bazaarvoice Jolt, is the reference for the transformation contract. The C++ implementation and its reference revision are documented in [NOTICE](NOTICE).

The library is distributed under the [Apache License 2.0](LICENSE). Its nlohmann/json dependency is distributed under the [MIT license](LICENSE.nlohmann-json). ICU is distributed under the terms in [LICENSE.ICU](LICENSE.ICU).
