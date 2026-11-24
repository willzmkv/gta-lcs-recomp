# gta-lcs-recomp — GTA: Liberty City Stories nativo para PC

Port de *GTA: Liberty City Stories* para Windows (D3D12) que executa o `libGame.so` original
da versão Android num JIT ARM64 → x86-64 (Dynarmic), com assets Retina do iPad, efeitos do
PS2, HD World (geometria detalhada até ~500 m), mira livre, legendas, menu global e launcher
próprio. Versão: `0.1.0-alpha` (arquivo `VERSION`).

> **Agentes e desenvolvedores: comecem por [docs/HANDOFF.md](docs/HANDOFF.md).** Ele traz o
> estado atual, regras de isolamento desta pasta e os próximos passos.

## Documentação

| Documento | Conteúdo |
|---|---|
| [docs/HANDOFF.md](docs/HANDOFF.md) | Estado atual, o que foi feito, problemas abertos, próximos passos |
| [docs/ARQUITETURA.md](docs/ARQUITETURA.md) | Boot, JIT, HLE, threads, hooks, render, VFS, mapa de módulos, build |
| [docs/HD_WORLD.md](docs/HD_WORLD.md) | Streaming de células HD, distâncias, LOD, população |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | Medições, causas de stutter, plano de paralelismo |
| [docs/DIAGNOSTICO.md](docs/DIAGNOSTICO.md) | Variáveis `LCS_DIAG_*`, rig de cruzeiro, instrumentação, artefatos |
| [docs/CONFIGURACAO.md](docs/CONFIGURACAO.md) | Chaves do `lcs.ini` |
| [AGENTS.md](AGENTS.md) | Invariantes de arquitetura e padrões de código |

## Arquivos do jogo (fornecidos pelo usuário, nunca versionados)

- Android: `base.apk` / `libGame.so` / `data_main.wad` / `data_music.wad` → `gtalcs/gamefiles/`
- iPad: `GTALcs.ipa` → extraído em `assets/ipad/`

`python tools/doctor.py` verifica se tudo está no lugar. Nesta máquina, `assets/`, `gtalcs/`
e `modloader/` são junctions para `D:\Games\LibertyStories` — **somente leitura**.

## Build e execução

```powershell
python tools/doctor.py            # ambiente e arquivos
python tools/build.py             # Release + LTO → lcs.exe
python tools/build.py --quick     # sem LTO (iteração rápida)
python tools/build.py --launcher  # LCSLauncher.exe
.\LCSLauncher.exe                 # ou .\lcs.exe direto
```

Requer Visual Studio com C++ (MSVC/clang-cl), CMake e Python 3. Todas as bibliotecas
(Dynarmic, Boost header-only, SDL3, ImGui, stb) estão em `third_party/` e são compiladas pelo
próprio build — nenhuma pasta externa é necessária.

## Estrutura

```
src/        código do lcs.exe (core, cpu, os, graphics, audio, input, vfs, game, ui, settings)
launcher/   LCSLauncher.exe
tools/      build, doctor, extração de assets, scripts de disassembly
docs/       documentação
third_party/ imgui, stb
scratch/    artefatos de diagnóstico (temporários, não versionar)
build/      saída do CMake
```
