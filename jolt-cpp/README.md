# Biblioteca Jolt C++

Biblioteca estática C++20 com API pública em [include/apolo/jolt.hpp](include/apolo/jolt.hpp). Recebe o documento e a especificação Chainr como JSON textual e devolve o JSON transformado.

## Referência e licença

O [Jolt Community](https://github.com/jolt-community/jolt-community) é um fork Apache-2.0 do Jolt da Bazaarvoice. Na comparação dos repositórios, o Community tinha desenvolvimento mais recente, releases próprias e 174 fixtures JSON de teste no núcleo, contra 158 no original. Ele foi escolhido como referência para a sintaxe e o comportamento. A [revisão consultada](https://github.com/jolt-community/jolt-community/tree/401b1df4ec61f6b66e498b28e2d3c67de21274f7) está registrada em [NOTICE](NOTICE). O Jolt Java 0.1.1 continua sendo a referência de compatibilidade do Apolo via JNI.

A biblioteca nativa é licenciada sob [Apache-2.0](LICENSE). A dependência nlohmann/json usa a [licença MIT](LICENSE.nlohmann-json). A instalação da biblioteca e a imagem Docker incluem os avisos aplicáveis.

## Cobertura

| Operação | Sintaxe implementada |
| --- | --- |
| `shift` | Chaves literais, alternativas literais, um curinga por padrão, caminhos aninhados de saída, `&n`, `&(n,1)`, índices, `[]`, `[#n]` e referências `@`, `@(n,caminho)`, `$n` e `#valor`. |
| `default`, `remove`, `cardinality` | Edição de árvores com chaves literais, índices explícitos de arrays existentes e curinga simples. Alternativas literais em `remove` e cardinalidade `ONE` e `MANY`. |
| `modify-overwrite-beta`, `modify-default-beta`, `modify-define-beta` | Subconjunto com valores literais e caminhos aninhados. |
| `sort` | Ordenação recursiva de chaves de objetos. |

A cobertura do dialeto é parcial. Funções de `modify`, índices por transposição, padrões de múltiplos curingas, classes Java e operações adicionais como `enrich` ficam fora do motor nativo. Use a variante JNI para especificações que precisem da cobertura Java disponível no Apolo. O motor C++ tem limites próprios de tamanho, profundidade, índices e trabalho por transformação.

## Compilar e usar

Pré-requisitos de build: CMake, compilador C++20 e pacote de desenvolvimento nlohmann/json 3.11 ou superior.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$PWD/build/install"
```

O build produz `libjolt_cpp.a`. Em CMake, use o alvo `jolt_cpp` ou ligue a biblioteca estática e adicione o diretório `include` ao projeto.

```cpp
#include <apolo/jolt.hpp>

auto result = apolo::jolt::transform(
    R"({"title":"Café"})",
    R"([{"operation":"shift","spec":{"title":"produto.nome"}}])");
```
