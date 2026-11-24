# Dynarmic (vendorizado)

- Origem: <https://github.com/azahar-emu/dynarmic> (continuação mantida do dynarmic de
  merryhime; o repositório original não está mais disponível no GitHub).
- Commit: `a46601580d5512d324104f985b5f0209dc980ddc` (2026-09-26), versão **6.7.0**.
- Licença: 0BSD (`LICENSE.txt`); externals mantêm suas próprias licenças.
- Submódulos incluídos como arquivos comuns (`externals/`): fmt `7b273fb`, mcl `0.1.14`,
  oaknut `94c726c`, robin-map `054ec5a`, xbyak `8c0098b`, zycore `0b2432c`, zydis `bffbb61`.
- Removidos por não serem usados no build: `tests/`, `externals/catch`,
  `externals/biscuit` (RISC-V), `externals/zydis/{tests,msvc}`, `externals/zycore/tests`,
  metadados `.git*` / `.github`.
- Modificação local (única): `src/dynarmic/backend/x64/a64_interface.cpp` — `Run()` e
  `Step()` aceitam chamada reentrante (o `ASSERT(!is_executing)` do upstream abortava). O
  projeto chama código guest de dentro de callbacks HLE no mesmo core
  (`DynarmicCore::call_nested`, que salva/restaura o contexto). O build do Eden usado antes
  tinha o mesmo comportamento por compilar com asserts desligados. Marcado com
  `gta-lcs-recomp:` no código.

Por que não o dynarmic 7.x do Eden/Lichii: ele depende da biblioteca `common` inteira do
Eden (`common/assert.h`, `common/container/*`, logging), então não é separável.

Integração: `CMakeLists.txt` raiz faz `add_subdirectory(third_party/dynarmic)` com
`DYNARMIC_USE_BUNDLED_EXTERNALS=ON`, `DYNARMIC_FRONTENDS=A64` e Boost de `third_party/boost`.
