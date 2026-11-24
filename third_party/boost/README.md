# Boost (subconjunto header-only)

- Versão: **1.92.0** (`BOOST_VERSION 109200`), licença Boost Software License 1.0 (`LICENSE_1_0.txt`).
- Conteúdo: somente os 1.050 headers necessários ao Dynarmic — fechamento transitivo de
  `boost/icl/interval_map.hpp`, `boost/icl/interval_set.hpp`, `boost/variant.hpp` e
  `boost/variant/get.hpp`.
- `cmake/BoostConfig.cmake` expõe `Boost::headers` / `Boost::boost` para o
  `find_package(Boost)` do Dynarmic (o `CMakeLists.txt` raiz aponta `Boost_DIR` para cá).

Para atualizar: pegue uma release do Boost, recalcule o fechamento dos headers que o
Dynarmic inclui (`grep -rh '#include <boost/' third_party/dynarmic/src`) e substitua
`include/`.
