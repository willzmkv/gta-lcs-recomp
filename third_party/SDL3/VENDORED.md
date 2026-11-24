# SDL3 (vendorizado)

- Origem: release oficial <https://github.com/libsdl-org/SDL/releases/tag/release-3.4.20>
  (`SDL3-3.4.20.tar.gz`, publicada em 2026-11-03), licença zlib (`LICENSE.txt`).
- Removidos por não serem usados no build: `test/`, `examples/`, `docs/` (o README de docs
  ficou como `docs-README.md`), `Xcode/`, `VisualC/`, `VisualC-GDK/`, `android-project/`,
  e os arquivos de instruções para agentes do upstream (`AGENTS.md`, `CLAUDE.md`).
- Sem modificações no código-fonte.

Integração: `CMakeLists.txt` raiz faz `add_subdirectory(third_party/SDL3)` como biblioteca
estática (`SDL3::SDL3-static`); usado por `src/input/gamepad.cpp` e `src/audio/audio_system.cpp`.
