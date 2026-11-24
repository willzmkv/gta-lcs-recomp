# Performance — medições, gargalos e plano

Meta do usuário: **mais de 100 FPS sustentados** (< 10 ms por quadro) em toda Liberty City,
com tudo no máximo (`world.hd_radius=3`, distâncias e densidades 100, V-Sync e limite de FPS
desligados), **sem stutters**. Máquina de referência: AMD Ryzen 7 5800X (8 núcleos /
16 threads), AMD Radeon RX 7700 XT, 48 GB RAM, Windows 11.

O usuário autorizou explicitamente usar todo o paralelismo disponível — ILP, pipelining,
superescalaridade, múltipla emissão, multithreading, multicore — e skinning em CPU ou GPU.

---

## 1. Como medir

Rig de cruzeiro (detalhes em [DIAGNOSTICO.md](DIAGNOSTICO.md)):

```powershell
$env:LCS_DIAG_VERIFY = "1"; $env:LCS_DIAG_CRUISE = "1"; $env:LCS_DIAG_AUDIT = "1"
.\lcs.exe *> scratch\audit_N.log
```

O rig sai do menu, inicia o jogo, coloca o jogador num Kuruma e percorre uma rota fixa por
Portland (Saint Mark's, Chinatown, Bank, Trenton, Harbor) com teletransportes entre ilhas,
gravando capturas `scratch/cruise_*.bmp` e terminando com
`[VerifyDone] Cruise complete. peak_ms=… min_ms=… cell_caps=…`.

Com `LCS_DIAG_AUDIT=1` cada quadro imprime `[TMP_D3D]`:

| Campo | Significado |
|---|---|
| `wall` | Tempo total do quadro no host (ms) |
| `guest` | Tempo gasto executando código do jogo (JIT + HLE GL) no quadro |
| `draw` | Tempo gravando comandos D3D12 |
| `tex` / `n` | Tempo e quantidade de texturas criadas no D3D12 |
| `splits` | Vezes que o anel de upload (96 MB/quadro) encheu e forçou `split_command_list_and_wait` |
| `draws` | Draw calls do quadro |

Script de resumo usado neste documento (percentis de `wall`/`guest`):
ver [DIAGNOSTICO.md §4](DIAGNOSTICO.md#4-como-analisar-um-log).

> Compare sempre builds iguais. Os números abaixo são de build `--quick` (LTO desligado) com
> instrumentação ativa; o Release LTO deve ser algo mais rápido — **ainda não medido**.

## 2. Onde estamos (2026-11-23, `audit4.log`, build quick)

909 quadros medidos no cruzeiro, `hd_radius=3`, tudo em 100:

| Métrica | Valor |
|---|---|
| `wall` mediana | **13,24 ms (≈ 75 FPS)** |
| `wall` p95 / p99 / máx | 22,6 / 67,4 / 348 ms |
| `guest` mediana / p95 | **11,26** / 19,8 ms |
| `draw` D3D12 mediana | 1,09 ms |
| Draw calls mediana / máx | 3.313 / 5.641 |
| Quadros > 16,7 / > 33 / > 50 / > 100 ms | 174 / 19 / 15 / 7 |

`audit3.log` (mesmo build, rota mais longa com troca de ilha): mediana 13,53 ms, p99
52,2 ms, **máx 2.287 ms** (troca de ilha).

**Conclusão principal:** a GPU não é o gargalo (gravação D3D12 ≈ 1 ms, GPU folgada). O
gargalo é **uma única thread de CPU executando ~11 ms de código do jogo por quadro**, mais
trabalho síncrono do host (decode de textura, criação de recursos, HD World) nessa mesma
thread. Dos 16 threads do 5800X, o jogo usa efetivamente 1 para o caminho crítico.

### Fases do quadro (médias, `[TMP_PHASES]` no fim do `audit4`)

| Fase | Média (ms) | Observação |
|---|---|---|
| `CGame::Process` | 6,24 | Inclui scripts (`Scripts` média 2,65, com picos) |
| ↳ `CWorld::Process` | 2,33 | Varre as 100×100 listas de setores chamando `CEntity::UpdateDistanceFade` |
| ↳ `PopUpd` / `GenCars` / `StreamingUpd` | 0,19 / 0,09 / 0,38 | Picos grandes em troca de área |
| `ScanWorld` | 1,08 | Picos de 2,5..11 ms |
| `RenderScene` | 4,61 | `RenderBarRoads` 0,88, `RenderVehicles` 0,44, `cWorldStream::Render` ×6 0,64, `_renderDeferredWorld` ×6 0,86 |
| `RenderEffects` | 2,68 | Picos de 4..17 ms em chuva/Saint Mark's; sub-fases medidas somam pouco — custo ainda não atribuído |
| `Water` + `TransWater` | 0,70 | |
| HD World `rebuild_combined_chunk` | 2,3..6,8 por evento | 17 eventos > 1,2 ms em 909 quadros |

## 3. Causas dos stutters (medidas)

### 3.1 Texturas (stutters de 30..175 ms ao dirigir)

Medido com `[TMP_TEX_DECODE]` no `audit4`:

| Quadro | Texturas PVRTC | Megapixels | Decode | Criação D3D12 (`d3dPrep`) | Total |
|---|---|---|---|---|---|
| 98 (spawn) | 774 | 123,4 | 1.723 ms | 581 ms | 2.304 ms |
| 101 | 850 | 167,4 | 2.276 ms | 627 ms | 2.903 ms |
| 422 | 43 | 6,6 | 90 ms | 32 ms | 122 ms |
| 430 | 45 | 5,6 | 69 ms | 29 ms | 98 ms |
| 568 | 48 | 10,2 | 133 ms | 41 ms | 174 ms |

- **Decode PVRTC por software, single-thread:** ≈ **13–14 ms por megapixel**
  (`decode_pvrtc_rgba8`, `src/graphics/render_bridge.cpp`). O jogo (`uploadPVRTexture`,
  0x597de4) chama `glCompressedTexImage2D` para **cada nível de mip**, e cada chamada
  decodifica na hora, na RenderThread.
- **Criação de recurso por textura:** cada textura chama `CreateCommittedResource`
  (`prepare_d3d12_texture_entry`, `d3d12_backend.cpp` ~2690/2740) a ≈ **0,75–0,88 ms cada**.
- **Anel de upload cheio:** em rajadas o anel de 96 MB por quadro (`kUploadBytesPerFrame`,
  `d3d12_backend.cpp:35`) enche e força `split_command_list_and_wait` (espera a GPU).
- Texturas que voltam (célula/veículo evictado e recarregado) são **decodificadas de novo**
  — não há cache.

O VCS Recomp viu o mesmo padrão (rajadas de 100..400 texturas ao entrar em área nova) e
resolveu com threads auxiliares de criação de textura, mips na CPU e liberação assíncrona —
`D:\Games\vcs-hd-world-impl\docs\PERFORMANCE.md` §21 (stutters 365..448 ms → 45..48 ms).

### 3.2 HD World

- `step_streaming` gasta **68..79 ms** num quadro quando carrega várias células
  (`fread` do `.img` + `cRelocatableChunk::Fixup` executado no JIT para cada célula).
- `rebuild_combined_chunk` reconstrói **todo** o chunk combinado (~2.300..2.500 instâncias)
  a cada mudança: 2,3..6,8 ms.

### 3.3 Troca de ilha / teletransporte (400..2.300 ms)

Disparado em `CStreaming::LoadBigBuildingsWhenNeeded()` (0x4a83f0): apaga os assets da ilha
anterior (`bdel` 6.960..8.048 recursos), chama `cWorldStream::LoadLevel()` (lê e infla o
nível de forma síncrona) e depois `CStreaming::LoadAllRequestedModels(0)` (centenas de
modelos/TXDs num quadro), seguido de 224..850 texturas novas (decode + criação). O jogo
original mostra tela de carregamento nesse ponto; parte do custo é inerente, mas decode e
criação de textura são a maior fatia e são paralelizáveis.

## 4. Decisão em vigor: lógica e render numa thread só

O `libGame.so` móvel tem duas threads: **gGameTickThread** (lógica + gravação de comandos GL
numa fila, `lglRenderQueue`) e **RenderThread** (reproduz a fila). A sessão anterior mediu
(`scratch/diag_stages.log`, 2026-11-23 ~01:18):

- Com a fila ligada, a sincronização é *fork-join por quadro*
  (`lglThreadSync::tellWorkerThreadsToBegin` / `waitForWorkerThreadsToFinish`), não um
  pipeline entre quadros: a RenderThread dormia ~13 ms esperando, e a codificação +
  decodificação da fila custava ~10 ms (`FlushCmds 2 × 4,9 ms`).
- Por isso `requestEnableGameTickThread` (0x534cb0) foi desviado para o ramo "disable"
  (`src/game/lcs_distance.cpp:1761-1768`): tudo roda direto na RenderThread ("Direct-GL").
- Efeito colateral corrigido depois: `Display::UseVAOs()` checa `lglIsRenderQueueBound()` e,
  com a fila desligada, explodia o número de chamadas GL (bind buffer 1.778 → 9.745);
  `lcs_batched_fast.cpp` substitui esse caminho.

**Leitura atual:** a decisão removeu overhead, mas fixou o teto do jogo em
*lógica + render em série* (≈ 6 + 5 ms). Para passar de 100 FPS com folga, é preciso voltar
a sobrepor as duas metades — porém com um **pipeline de verdade** (quadro N+1 simulando
enquanto o quadro N é submetido), não com a fila original. Ver P3 abaixo.

## 5. Plano de paralelismo e otimização

Priorizado por ganho perceptível / risco. Cada item deve ter chave de desligar
(variável de ambiente ou opção) para A/B no rig de cruzeiro.

### P1 — Pipeline de texturas assíncrono (stutters 1 e 2)

**Etapa 1 implementada em 2026-11-24** (`src/graphics/texture_pipeline.*`,
`src/graphics/r_d3d12/d3d12_backend.cpp`):

- `glCompressedTexImage2D` só copia o payload comprimido e enfileira um job; um pool de
  workers (`TexDecode0..N`, prioridade abaixo do normal) calcula um hash de conteúdo de 64 bits
  em 4 trilhas independentes (ILP), consulta o cache LRU de texturas decodificadas (256 MB),
  decodifica se necessário e já obtém o recurso D3D12 (do pool ou criado no próprio worker —
  `ID3D12Device` é free-threaded).
- A RenderThread anexa jobs prontos a cada flush de upload. Um draw que amostra uma textura
  ainda pendente completa o job na hora (`lookup_tex_ready`), então nada é desenhado sem
  conteúdo.
- Pool de recursos RGBA8 por tamanho (256 MB): texturas aposentadas voltam ao pool depois da
  fence do quadro em vez de serem destruídas e recriadas (`CreateCommittedResource` ≈ 0,8 ms).
- Chaves: `LCS_TEX_WORKERS` (0 = caminho síncrono antigo), `LCS_TEX_CACHE_MB`.

Resultado no rig de cruzeiro (build quick, janela oculta, quadros 100+, mesmo binário):

| | síncrono (2 execuções) | pipeline, 8 workers | **pipeline, 4 workers (padrão)** |
|---|---|---|---|
| mediana | 12,75 / 12,77 ms | 13,69..14,08 ms | **12,92 ms** |
| p99 | 39,7 / 34,3 ms | 22,7..25,7 ms | **23,5 ms** |
| pior quadro | 269,7 / 91,9 ms | 36,9..39,4 ms | **44,0 ms** |
| quadros > 50 ms | 9 / 3 | 0 | **0** |
| quadros > 100 ms | 2 / 0 | 0 | **0** |
| tempo de criação de textura na RenderThread | 309..327 ms | 31..41 ms | **38 ms** |

Release com LTO (agora real), mesmo rig, quadros 100+:

| | síncrono | pipeline (4 workers) |
|---|---|---|
| mediana | 12,85 ms | 13,33 ms |
| p99 | 38,1 ms | **25,5 ms** |
| pior quadro | 60,7 ms | **48,9 ms** |
| quadros > 50 ms | 4 | **0** |

O LTO quase não altera a mediana (~12,8 ms em quick e em LTO): o tempo dominante é o código
do jogo no JIT, não o código do host. Os +0,5 ms de mediana com o pipeline estão perto do
ruído entre execuções (0,0..0,5 ms nas medições), mas devem ser acompanhados.

Por execução: ~6.670 texturas, ~240..250 servidas pelo cache, 2..12 esperas em draw,
0..1 execução inline. Com 8 workers a mediana piorou ~1 ms (disputa de núcleo/SMT/cache com as
threads do jogo); por isso o padrão é ¼ das threads (2..4).

Pendente nesta prioridade (itens abaixo): decode vetorizado (SSE/AVX2), transcodificação
para BC1/BC3 ou decode na GPU, destruição assíncrona, orçamento de upload por quadro.

1. **Cache por conteúdo:** hash rápido (XXH3/FNV de 64 bits) dos bytes PVRTC → RGBA8 já
   decodificado (LRU limitado em RAM). Textura recarregada custa ~0,01 ms em vez de 2..3 ms.
2. **Pool de decode:** `std::jthread` × (núcleos − 3) (≈ 5..13 no 5800X). `glCompressedTexImage2D`
   copia os bytes, enfileira o job e retorna; a textura fica com o conteúdo anterior/placeholder
   até ficar pronta (ou bloqueia só se for usada no mesmo quadro e ainda não terminou).
   Decodificar todos os mips de uma textura no mesmo job.
3. **Decode vetorizado:** o PVRTC decodifica blocos 4×4 independentes — SSE4/AVX2 em vários
   blocos por vez (ILP + SIMD). Meta: ≤ 3 ms/Mpx por thread.
4. **Alternativa a avaliar:** transcodificar PVRTC → BC1/BC3 (ou BC7) no worker e enviar
   comprimido: 4..8× menos memória de upload e VRAM. Ou decode em *compute shader* na GPU.
5. **Recursos *placed*:** heaps `ID3D12Heap` de 64 MB pré-alocados + reciclagem de
   `ID3D12Resource` por `(largura, altura, mips, formato)`. Elimina os ~0,8 ms por
   `CreateCommittedResource`.
6. **Destruição assíncrona:** fila de recursos liberados destruída por uma thread "reaper"
   depois da fence da GPU (o VCS mediu 160..200 ms de destruição em massa).
7. **Orçamento de upload:** uploads que não cabem no anel passam para o quadro seguinte em vez
   de `split_command_list_and_wait`.

Meta: nenhuma rajada de textura acima de ~5 ms na thread crítica; troca de ilha < 300 ms.

### P2 — HD World e população

1. **Streaming de células em worker:** `fread` + inflate + validação das relocações numa
   thread; o game thread só faz o que precisa do JIT (registro de modelos). Considerar
   `cRelocatableChunk::Fixup` nativo em C++ (a rotina está em `scratch/dis_hot4.txt`
   ~466..521) para tirar do JIT.
2. **Chunk combinado incremental:** adicionar/remover instâncias por célula em vez de
   reconstruir 2.500 instâncias a cada mudança.
3. **Despawn de carros distantes:** em `PossiblyRemoveVehicle` (0x3b4eb0), ignorar
   `GetIsOnScreen()` para carros aleatórios/não-missão além de ~380 m (com frustum de 3.200 m
   quase tudo está "na tela"). Respeitar `CVehicle::CanBeDeleted` (0x4f7218).

### P3 — Custo estável por quadro (≈ 11 ms → < 7 ms)

1. **Pipeline lógica/render entre quadros.** Gravar as chamadas GL/HLE do quadro N num buffer
   de comandos do host (no `RenderBridge`) e reproduzi-las em D3D12 numa thread de render
   enquanto o jogo já simula o quadro N+1. É o equivalente ao `RenderWorker` do VCS
   (26–29 → 44–54 FPS só com essa mudança lá). Cuidado: leituras de estado GL de volta pelo
   jogo (glGet*, queries) precisam ser respondidas sem esperar o render.
2. **Nativizar pontos quentes** (substituir via hook GOT por C++ equivalente, validando com
   capturas): `CWorld::Process` / `UpdateDistanceFade` (varredura de 10.000 setores),
   `CRenderer::ScanWorld`, `RenderEverythingBarRoads`, o laço de matrizes de osso em
   `ElementDefaultRenderCallBack` (0x51e5bc — o jogo já faz skinning na GPU com até 64 ossos;
   só a multiplicação de matrizes roda no JIT) e o que restar em `RenderEffects`.
3. **Dynarmic:** hoje usa as otimizações seguras padrão (`src/cpu/dynarmic_core.cpp:330-344`,
   fastmem 1:1). Testar `unsafe_optimizations` com flags de FP (`Unsafe_UnfuseFMA`,
   `Unsafe_ReducedErrorFP`, `Unsafe_InaccurateNaN`, `Unsafe_IgnoreStandardFPCRValue`) — usadas
   por emuladores de Switch para ganhos grandes em código float. Validar física/replays.
4. **Paralelismo de dados dentro do quadro:** animação de peds/veículos em fatias por worker
   (o VCS limitou a 3 workers; mais roubava núcleo das threads principais).

### P4 — Acabamento

- Build Release LTO e, se possível, PGO.
- Remover toda a instrumentação `TMP_*` (lista em DIAGNOSTICO.md §3).

## 6. Histórico do cruzeiro (min/pico por execução)

Extraído das linhas `[VerifyDone]` em `scratch/*.log` (build e rota variaram entre execuções;
serve para ver tendências, não para comparar valores isolados).

| Data/hora | Log | min ms | pico ms | Nota |
|---|---|---|---|---|
| 11-22 19:36 | `diag_pop_baseline` | 10,85 | 20,5 | Antes do trabalho de população |
| 11-22 21:23 | `diag_pop_big2` | 12,16 | 48,7 | População ampliada |
| 11-23 01:18 | `diag_stages` | 11,91 | 31,7 | GameTickThread ligada |
| 11-23 01:23 | `diag_st` | 12,83 | 78,9 | Primeiro teste Direct-GL |
| 11-23 02:27 | `diag_batched` | 9,71 | 98,4 | `lcs_batched_fast` |
| 11-23 10:46 | `diag_opt2_noprof` | 7,54 | 34,3 | Sem profiler |
| 11-23 12:39 | `LCS_BF_NO_MODEL` | 6,99 | 29,9 | A/B do caminho rápido |
| 11-23 14:13 | `audit3` | 3,81 | 1.146 | Rota longa com troca de ilha |
| 11-23 14:38 | `audit4` | 8,54 | 31,3 | Última sonda |
