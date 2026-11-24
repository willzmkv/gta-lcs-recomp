# Arquitetura — gta-lcs-recomp (GTA: Liberty City Stories para PC)

Port nativo para Windows/Linux que executa o `libGame.so` **original** da versão Android
(arm64-v8a) dentro de um JIT ARM64 → x86-64, combinando assets do Android e do iPad.
Não é uma decompilação: o binário do jogo roda praticamente intacto, e o projeto fornece
o "sistema operacional" mínimo ao redor dele (libc, pthreads, JNI, OpenAL, GLES/EGL) e
substitui partes quentes ou problemáticas por código nativo.

Invariantes do projeto estão em `AGENTS.md` (D3D12/Vulkan, sem OpenGL no PC, assets nunca no
git, todo acesso a arquivo via `LogicalFS`).

---

## 1. Visão geral

```
LCSLauncher.exe ──(lcs.ini)──► lcs.exe
                                 │
   ┌─────────────────────────────┼──────────────────────────────────┐
   │ ElfLoader (src/cpu/elf_*)   │ mapeia libGame.so, libopenal.so,  │
   │                             │ libVendor_mpg123.so em 0x7f8…     │
   │ HLEManager (callbacks.cpp)  │ símbolos externos → trampolins SVC │
   │ DynarmicCore (dynarmic_*)   │ JIT A64, um core por thread guest  │
   │ ThreadManager (src/os)      │ pthread guest = std::thread host   │
   │ JNIBridge (src/os)          │ JNI_OnLoad + métodos Java falsos   │
   ├─────────────────────────────┼──────────────────────────────────┤
   │ RenderBridge (src/graphics) │ EGL/GLES → abstração → D3D12      │
   │ OpenALBridge (src/audio)    │ OpenAL Soft guest → mixer host    │
   │ LogicalFS / ModLoader (vfs) │ modloader/ → assets/ipad/ → WADs  │
   │ Input (src/input)           │ teclado, mouse, XInput, DualSense │
   ├─────────────────────────────┴──────────────────────────────────┤
   │ Módulos de jogo (src/game): hooks GOT + patches de instrução    │
   │ UI (src/ui): ImGui — menu global, trainer, configurações        │
   └────────────────────────────────────────────────────────────────┘
```

## 2. Sequência de inicialização (`src/core/main.cpp`)

O log imprime cada etapa como `[Step N]`:

1. **Step 1** — carrega os ELFs AArch64 em memória virtual (endereços fixos:
   `libGame.so` em `0x7f80000000`, `libopenal.so` em `0x7f7a000000`, mpg123 em `0x7f81800000`).
   Host e guest compartilham o mesmo espaço de endereços (identidade 1:1).
2. **Step 2** — verifica exportações (`JNI_OnLoad`, `Java_com_rockstargames_gtalcs_GTAJNIlib_*`,
   `_LogicalFS_Init`, …).
3. **Step 3** — aplica relocações (`RELATIVE`, `ABS64`, `GLOB_DAT`, `JUMP_SLOT`). Símbolos
   externos (libc, libm, GLES, EGL, OpenAL, Android) são resolvidos pelo `HLEManager` para
   trampolins em `0x7f00200000` (até 8.192) que executam `SVC #id`; o Dynarmic intercepta o SVC
   e chama a implementação C++ correspondente.
4. **Step 4** — lista construtores `.init_array` (165 no `libGame.so`).
5. **Step 5** — inicia o Dynarmic, roda autotestes de JIT/SVC, aplica patches de timing
   (`CTimer::Update`, `cTime::Refresh`, cutscene, velocidade de ped em FPS alto) e instala os
   módulos de jogo: `LCSFreeAim`, `LCSPs2Effects`, legendas, `LcsDistance`, `LcsMemory`,
   `LcsWorldHd` (main.cpp ~1736-1748).
6. **Step 6** — executa os construtores guest.
7. **Step 7** — inicia o JNI 1.6 e chama `JNI_OnLoad`.
8. **Step 8** — sequência de bootstrap Android (setAssetPacksInfo, setPrivateFilesDir, …),
   controles.
9. **Step 9** — janela Win32, swapchain D3D12, áudio; `RenderBridge::initialize`; hooks de
   briefing do trainer. Daí em diante o laço chama `viewOnDrawFrame` a cada quadro.

## 3. CPU e threads

- **Dynarmic** (`src/cpu/dynarmic_core.cpp`): fastmem com ponteiro base 0 (acesso direto à
  memória host), acessos inválidos tratados por SEH, sem contagem de ciclos, `CNTPCT` real.
  Otimizações *unsafe* de FP **não** estão ligadas. A biblioteca vem de
  `DYNARMIC_ROOT = D:/Games/Lichii-nx2/Lichii-nx2` (`CMakeLists.txt:201`).
- **Uma thread guest = uma thread host** (`src/os/thread_manager.cpp`), cada uma com seu core
  Dynarmic, pilha e TLS. Mutex/condvar/semáforo guest são mapeados para primitivas host por
  endereço.
- Threads relevantes do jogo: **RenderThread** (chama `viewOnDrawFrame`; hoje roda lógica e
  render — ver abaixo), **gGameTickThread** (lógica; desativada), **cUmdThread** / StreamThread
  (leitura de disco assíncrona), threads de áudio.
- **Thread única para lógica + render:** `requestEnableGameTickThread` foi desviado para o
  ramo "disable" (`src/game/lcs_distance.cpp:1761-1768`). Motivo e consequências em
  [PERFORMANCE.md §4](PERFORMANCE.md#4-decisão-em-vigor-lógica-e-render-numa-thread-só).
- O host **não** tem pools de workers para decode/upload; só o driver de gamepad
  (`src/input/gamepad.cpp:590`) cria thread própria.

### Chamando código do jogo a partir do host

`DynarmicCore::call` / `call_nested` executam uma função guest a partir de C++. `call_nested`
salva e restaura registradores (inclusive V0..V31, PSTATE, FPCR, FPSR) para poder ser usado
dentro de um callback que já está no meio de uma execução guest. Regra: chamar código guest
**somente** na thread que é dona daquele core (o core ativo é `DynarmicCore::current()`).

## 4. Como os módulos de jogo se conectam

Três técnicas, combinadas:

1. **Hook de GOT** — `patch_got_entry(cpu, load_bias, got_rva, novo_alvo, …)` troca o ponteiro
   de uma entrada da GOT do `libGame.so` por um stub; o original é guardado para encadear.
   As RVAs são constantes verificadas no topo de cada módulo (ex.: `lcs_world_hd.cpp:40-60`).
2. **Stubs ARM64 gerados em runtime** — uma página executável recebe pequenos programas A64
   (salvam registradores, `blr` para um callback HLE "pre", chamam o original, `blr` para o
   "post", restauram). Ver `HookOps` em `lcs_world_hd.cpp` / `lcs_distance.cpp`.
3. **Patch de instrução** — reescrita de constantes imediatas (`mov w9, #…, lsl #16`) e desvios
   no `.text`, sempre seguida de `cpu.invalidate_cache(va, size)`. Ex.: distâncias de spawn de
   carro em `lcs_distance.cpp:1750-1759`.

Toda RVA vem de disassembly do `libGame.so` (llvm-objdump; os dumps usados estão em
`scratch/dis_*.txt` / `obj_*.txt`).

## 5. Render

- `RenderBridge` (`src/graphics/render_bridge.cpp`) implementa EGL e GLES (estado, buffers,
  shaders, texturas, draws) sobre uma abstração com backend **Direct3D 12**
  (`src/graphics/r_d3d12/d3d12_backend.cpp`). O backend Vulkan existe só como esqueleto
  (`src/graphics/r_vulkan/*.cpp` têm poucas linhas).
- Shaders GLSL ES do jogo são traduzidos em `shader_compiler.cpp`.
- Texturas PVRTC/DXT/ETC são **decodificadas para RGBA8 na CPU** (`decode_pvrtc_rgba8`,
  `decode_compressed_to_rgba8`) e enviadas por um anel de upload de 96 MB por quadro
  (3 quadros em voo, `kFrameCount = 3`).
- `lcs_batched_fast.cpp` substitui `BatchedWorld::C_WorldRenderManager::Render` e
  `BatchedModel::C_DeferredBatchManager::Render` por código x64 que chama o `RenderBridge`
  direto (sem JIT, sem buscas em árvore por draw, sem binds redundantes).
- `lcs_ps2_effects.cpp` reproduz efeitos do PS2 (timecycle do `GAME.DTZ`, radiosity/bloom,
  filtro de cor `CMBlur`, rastros, motion blur em alta velocidade) na fronteira 3D→2D.
- Sobreposição ImGui (`src/ui/imgui_overlay.cpp`) para menu global, configurações e trainer.

## 6. Arquivos e mods

`LogicalFS` (`src/vfs/logical_fs.cpp`) resolve cada caminho pedido pelo jogo nesta ordem:
`modloader/<mod>/` → `assets/ipad/` (assets Retina do iPad) → WADs base (`data_main.wad`).
O `ModLoader` indexa `modloader/` uma vez (prioridade por pasta). Ao sair, o log mostra
`[LogicalFS] Session asset hits: modloader=…, ipad_retina=…`.

## 7. Mapa de módulos

| Pasta / arquivo | Função |
|---|---|
| `src/core/main.cpp` | Entrada, boot em 9 etapas, laço principal, rig de verificação `LCS_DIAG_*` |
| `src/core/config.*` | Configuração de baixo nível (`g_config`) |
| `src/cpu/` | ELF loader, Dynarmic, HLE de símbolos externos (`callbacks.cpp`, inclui GLES) |
| `src/os/` | Bionic libc/libm, threads, JNI |
| `src/graphics/` | `RenderBridge`, D3D12, tradutor de shaders, janela, vídeo |
| `src/audio/` | Ponte OpenAL e mixer |
| `src/input/` | Teclado/mouse, câmera por mouse, gamepads, perfis de controle |
| `src/vfs/` | LogicalFS, WAD, ModLoader |
| `src/settings/` | `lcs.ini` compartilhado com o launcher (ver [CONFIGURACAO.md](CONFIGURACAO.md)) |
| `src/ui/` | Menu global (F2), configurações, trainer, localização em 8 idiomas |
| `src/game/lcs_world_hd.*` | HD World: células extras de geometria detalhada ([HD_WORLD.md](HD_WORLD.md)) |
| `src/game/lcs_distance.*` | Distância de visão/LOD, população, pools, spawn de carros, neblina |
| `src/game/lcs_memory.*` | Política de retenção de streaming conforme `world.streaming_memory` |
| `src/game/lcs_batched_fast.*` | Render nativo dos lotes de mundo/modelos |
| `src/game/lcs_physics_timing.*` | Correções de física de veículos em FPS alto |
| `src/game/lcs_free_aim.*` | Mira livre 1:1 com mouse/analógico |
| `src/game/lcs_prompts.*` | Ícones de botão dinâmicos (teclado / PlayStation / Xbox / Switch) |
| `src/game/lcs_subtitles.*`, `lcs_dialogue_speakers.*` | Legendas com fonte Oswald, nome de quem fala |
| `src/game/lcs_ps2_effects.*`, `ps2_timecyc_data.h` | Pipeline de efeitos do PS2 |
| `src/game/lcs_trainer.*` | Trainer e ponte de estado do jogo |
| `launcher/` | `LCSLauncher.exe` (D3D11/Vulkan, UI própria, checagens, build) |
| `tools/` | `build.py`, `doctor.py`, extração de assets, scripts de disassembly |

## 8. Build

```powershell
python tools/doctor.py              # checa compilador, CPU, memória, arquivos do jogo
python tools/build.py               # Release com LTO → lcs.exe na raiz
python tools/build.py --quick       # sem LTO (compila mais rápido, roda mais devagar)
python tools/build.py --launcher    # LCSLauncher.exe
python tools/build.py --clean       # apaga build/ antes
```

CMake + Visual Studio (MSVC/clang-cl), C++20. Opções em `CMakeLists.txt`: `ENABLE_D3D12`,
`ENABLE_VULKAN`, `ENABLE_LTO`, `ENABLE_TESTS`. Dependências em `third_party/` (ImGui, stb) e
Dynarmic externo (seção 3).
