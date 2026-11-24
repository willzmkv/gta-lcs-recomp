# Diagnóstico — variáveis de ambiente, rig de verificação e instrumentação

## 1. Variáveis de ambiente

Todas são lidas por `lcs.exe` (`src/core/main.cpp`, salvo indicação).

| Variável | Efeito |
|---|---|
| `LCS_DIAG_VERIFY=1` | Liga o rig automático: navega menus, tira capturas em `scratch/verify_*.bmp`, sai sozinho. Sem ela, `main.cpp:114` segue o fluxo normal |
| `LCS_DIAG_CRUISE=1` | Com `VERIFY`: inicia jogo, põe o jogador num Kuruma e percorre a rota de Portland com teletransportes; capturas `scratch/cruise_*.bmp`; termina com `[VerifyDone] Cruise complete. peak_ms= min_ms= cell_caps=` |
| `LCS_DIAG_CRUISE_FRAMES=N` | Duração da etapa de direção (padrão 620, mínimo 61) |
| `LCS_DIAG_AUDIT=1` | Ativa toda a instrumentação temporária `TMP_*` (seção 3) |
| `LCS_DIAG_BOOT_ONLY=1` | Com `VERIFY`: para após checar boot + ESC |
| `LCS_DIAG_VERIFY_HD=1` | Com `VERIFY`: captura a aba Gráficos & Mundo e segue para o teste HD |
| `LCS_DIAG_SUB_LANG=0..7` | Com `VERIFY`: novo jogo no idioma indicado para testar legendas/briefs |
| `LCS_DIAG_CUTSCENE=1` | Diagnóstico de cutscenes |
| `LCS_DIAG_NO_PS2=1` | Com cutscene: força `ps2.pipeline=0` |
| `LCS_DIAG_FPS=N` | Com cutscene: força limite de FPS |
| `LCS_DIAG_REVERT_PC=1` | Com cutscene: desfaz o patch de `CCutsceneObject::ProcessControl` |
| `LCS_DIAG_EARLY=1` | Com cutscene: despeja objetos da cena nos primeiros quadros de cada cutscene |
| `LCS_DIAG_NO_IPAD=1` | Desliga a camada `assets/ipad/` no LogicalFS (`src/vfs/logical_fs.cpp:187`) |
| `LCS_PHYSICS_TIMESTEP=0` | Desliga as correções de física em FPS alto (`src/game/lcs_physics_timing.cpp:82`) |
| `LCS_FOOT_WALL=0` | Desliga a correção da câmera a pé contra paredes (`src/game/lcs_free_aim.cpp:721`) |

Exemplo (PowerShell):

```powershell
$env:LCS_DIAG_VERIFY="1"; $env:LCS_DIAG_CRUISE="1"; $env:LCS_DIAG_AUDIT="1"
.\lcs.exe *> scratch\audit_N.log
Remove-Item Env:LCS_DIAG_VERIFY, Env:LCS_DIAG_CRUISE, Env:LCS_DIAG_AUDIT
```

O rig respeita o `lcs.ini` do usuário (não o sobrescreve). Não rode enquanto o usuário
estiver jogando.

## 2. Tags úteis no log

| Tag | Origem | Conteúdo |
|---|---|---|
| `[Step N]` | `main.cpp` | Etapas de boot |
| `[THREAD] pthread_create` | `thread_manager.cpp` | Threads guest criadas |
| `[VerifyDone]` | `main.cpp:3218` | Fim do rig, com `peak_ms`/`min_ms` |
| `[Result] Render loop exited after N frames` | `main.cpp` | Totais de draws, shaders, texturas, buffers |
| `[LogicalFS] Session asset hits` | `main.cpp` | Quantos assets vieram de mods / iPad |
| `[D3D12] WARN: split_command_list_and_wait!` | `d3d12_backend.cpp:540` | Anel de upload cheio (stall de GPU) |

## 3. Instrumentação temporária ainda no código

Toda ela só imprime com `LCS_DIAG_AUDIT=1`, mas **deve ser removida antes de entregar**
(checagem: buscar `TMP_` e `LCS_DIAG_AUDIT` em `src/` deve dar zero).

| Tag | Arquivo:linha | Mede |
|---|---|---|
| `[TMP_D3D]` | `src/graphics/r_d3d12/d3d12_backend.cpp:398` (contadores), `:1918` (liga), `:4139` (imprime) | Tempo de quadro, guest, draw, texturas, splits |
| `[TMP_TEX_DECODE]` | `src/graphics/render_bridge.cpp:591-605`, `:1520`, `:1628` | Decode PVRTC/glTex e preparação D3D12 por quadro |
| `[TMP_PHASES]`, `[TMP_PHASES_SUB]`, `[TMP_PHASE_PROBE]` | `src/game/lcs_distance.cpp:994-1185` | Fases de `CGame::Process`, `RenderScene`, `RenderEffects`, streaming (stubs na página de hooks) |
| `[TMP_CAR_SPAWN]` | `src/game/lcs_distance.cpp:1145` | Distância de cada spawn aleatório |
| `[TMP_LOD_AUDIT]`, `[TMP_LOD_SUM]` | `src/game/lcs_distance.cpp:1636-1699` | LOD gêmeo visível sobre área HD |
| `[TMP_CAR_AUDIT]`, `[TMP_CAR_STUCK]` | `src/core/main.cpp:3115-3169` | Contagem de carros/nós e carros > 500 m |
| `[TMP_WHD]`, `[TMP_WHD_EVICT]` | `src/game/lcs_world_hd.cpp:1719`, `:2287-2366` | Custo do HD World por quadro e evicção |
| `[TMP_BUCKET]`, `[TMP_WORLD_FAST]`, `[TMP_MODEL_FAST]` | `src/game/lcs_batched_fast.cpp:1497-2013` | Custo do render nativo de lotes |

Também: a variável local `ev_scan_ms` em `lcs_world_hd.cpp` gera aviso C4189.

## 4. Como analisar um log

Percentis de tempo de quadro a partir das linhas `[TMP_D3D]`:

```python
import re, sys
w, g = [], []
for l in open(sys.argv[1], encoding="utf-8", errors="replace"):
    m = re.search(r"\[TMP_D3D\] f=\d+ draws=\d+ wall=([\d.]+) guest=([\d.]+)", l)
    if m: w.append(float(m[1])); g.append(float(m[2]))
p = lambda a, q: sorted(a)[int(q * (len(a) - 1))]
print(f"n={len(w)} wall p50={p(w,.5):.2f} p95={p(w,.95):.2f} p99={p(w,.99):.2f} max={max(w):.1f}")
print(f"guest p50={p(g,.5):.2f}  >33ms={sum(x>33 for x in w)}  >50ms={sum(x>50 for x in w)}")
```

Para ver de onde vem um pico: procure o `f=` do quadro lento e as linhas `[TMP_TEX_DECODE]`,
`[TMP_WHD]` e `[TMP_PHASES]` próximas (`pf=` é o contador do probe de fases, não o mesmo
número de `f=`).

## 5. Artefatos em `scratch/`

Gerados pela sessão anterior (2026-11-21..23). Os mais úteis:

| Arquivo | O que é |
|---|---|
| `audit3.log`, `audit4.log` | Últimas sondas completas (fases, texturas, D3D12, carros, LOD) |
| `spikes3.txt` | Picos extraídos do `audit3` |
| `diag_stages.log`, `diag_st.log` | Comparação GameTickThread ligada × thread única |
| `dis_step1.txt`, `dis_step1b.txt` | Disassembly de streaming, `ScanWorld`, `RenderVehicles`, `CanBeDeleted`, `LoadAllRequestedModels` |
| `dis_hot4.txt` | `RenderEffects`, `CStreaming::Update`, `RemoveDistantCars`, `cRelocatableChunk::Fixup` |
| `dis_matt_pvr.txt` | `MattRenderScene`, `uploadPVRTexture`, `CWorld::Process`, `cWorldStream::LoadLevel` |
| `dis_elem_cb.txt`, `dis_skin_lgl.txt` | `ElementDefaultRenderCallBack` (skinning), `lglTextureManager`, `RslQuat*`, `StreamThread` |
| `dis_cgame_lgl.txt` | `CGame::Process`, `RenderScene`, `lglTextureLoader` |
| `relocs.txt` | Todas as relocações do `libGame.so` (3 MB) |
| `cruise_*.bmp/png` | Capturas do rig (bank/china/stmarks com hd0 vs hd3, células 0..5) |

Disassembly: `llvm-objdump` do ROCm (`C:\Program Files\AMD\ROCm\7.1\bin\llvm-objdump.exe`)
não reconhece aarch64 por padrão — é preciso passar `--triple=aarch64-linux-android`. O
`libGame.so` fica em `gtalcs\gamefiles\android-libs\libGame.so` (junction, somente leitura).

## 6. Histórico das sessões de agentes

As transcrições das sessões anteriores (Antigravity/Gemini) ficam em
`C:\Users\dejot\.gemini\antigravity-cli\brain\<id>\.system_generated\logs\transcript_full.jsonl`
(um JSON por linha com `type`, `created_at`, `content`). Sessões deste projeto:

| ID | Período | Tema |
|---|---|---|
| `eacd28dd-…` | 11-20 19:49 → 11-22 16:52 | Launcher, menu global/principal, prompts, legendas, crashes HD World |
| `89aca45b-…` | 11-21 02:31 → 11-21 11:55 | Prompts para outros agentes, efeitos PS2, upscale de artworks |
| `088c687b-…` | 11-22 17:27 → 11-23 14:48 | Pop-in, população, performance, stutters (parou sem concluir) |

Os resumos de *checkpoint* (`type == "CHECKPOINT"`) trazem o estado completo a cada
compactação; o último está no passo 5174.
