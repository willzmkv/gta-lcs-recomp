# HD World, distâncias e população

Objetivo: ver a cidade com geometria detalhada (iPad/mobile) muito além da janela original
do jogo, sem pop-in de prédios/props, sem sobreposição de LOD borrado com o HD, com trânsito
e pedestres coerentes ao dirigir. Inspirado na implementação do VCS Recomp
(`D:\Games\vcs-hd-world-impl\docs\HD_WORLD_STREAMING_HANDOFF.md`), mas o mecanismo aqui é
outro: LCS roda o `libGame.so` Android, então tudo é feito por hooks sobre `cWorldStream`.

São três módulos que trabalham juntos:

| Módulo | Papel |
|---|---|
| `src/game/lcs_world_hd.cpp` | Carrega e desenha **células extras** de geometria estática detalhada |
| `src/game/lcs_distance.cpp` | Distância de visão, LOD, culling, população, pools, spawn, neblina |
| `src/game/lcs_memory.cpp` | Quanto o streaming retém em memória (`world.streaming_memory`) |

---

## 1. Como o jogo original organiza o mundo

- Cada ilha é um nível: `Models/indust.img` (Portland, nível 1), `commer.img` (Staunton, 2),
  `suburb.img` (Shoreside Vale, 3), `underg.img` (subterrâneo, 4).
- O nível é dividido numa **grade hexagonal de células de 100 × 86,6 unidades**, 45 linhas,
  com origem em (−2000, −2000) (`world_to_hex_cell` / `hex_cell_center`,
  `lcs_world_hd.cpp:206-215`).
- `cWorldStream` mantém só a célula atual (e a anterior, para crossfade) residente; o resto da
  cidade é desenhado com modelos LOD de baixo detalhe (`LOD*`, big buildings).
- Leitura de disco é assíncrona via `cUmdThread` (`UmdReadAsync`), depois
  `cRelocatableChunk::Fixup` e `QueueBuildingSwaps`.

## 2. O que o `LcsWorldHd` faz

1. **Hooks GOT** em `cWorldStream::{Render, Update, LoadLevel, LoadScene, RemoveOldSectorModels,
   SwapToStreamedSector, SwapInStreamingBuffer, Stream, PollStreaming, CancelStreaming,
   StreamingCallback}`, `lglWorldManager::destroy` e `lglTextureManager::{getTexture,
   destroyTexture}` (RVAs em `lcs_world_hd.cpp:40-60`).
2. **Streaming próprio** (`step_streaming`, chamado em `on_world_update_post`, na thread do
   jogo): calcula a célula da câmera, lista células da grade até a distância de *preload*,
   ordena por distância e carrega até `max_loads` por quadro (3 se já houver ≥ 12 células, senão 8)
   lendo do `.img` do nível para uma arena própria de 134 MB (`kCellPoolBytes`), com
   validação da tabela de relocações antes do `Fixup`. Cache LRU de até 160 células
   (`kMaxCachedCells`).
3. **Registro de modelos** (`ensure_pairs_and_models`) e espera até cada célula estar pronta na
   GPU (`gpu_ready`).
4. **Chunk combinado** (`rebuild_combined_chunk`): junta as instâncias de todas as células
   ativas num único chunk em buffer duplo (`combined_chunk_bufs`, índice publicado/renderizado).
5. **Render** (`on_world_render_pre`/`post`): durante `cWorldStream::Render`, troca
   temporariamente `m_curChunks` pelo chunk combinado (com pré-culling de frustum feito em C++),
   zera o timer de crossfade e esvazia a fatia anterior; no `post` restaura tudo. Não é feita
   nenhuma chamada guest nem mutação de célula na RenderThread. A passada de sombras dinâmicas
   não recebe as células extras.
6. **Props** (`prestream_sector_props`, `sync_prop_lod_distances`): pede com antecedência os
   modelos dinâmicos dos setores próximos e ajusta distâncias de LOD dos props, para reduzir
   pop-in de postes, árvores, cones.
7. **Cobertura** (`is_point_covered_by_hd`, `is_circle_covered_by_hd`): usado pelo
   `LcsDistance` para esconder o "gêmeo" LOD borrado quando a área já está coberta por HD.

### Distâncias por raio (`world.hd_radius`)

| Raio | Ativa (desenha) | Preload | Evicção |
|---|---|---|---|
| 0 | desligado (comportamento original) | — | — |
| 1 | 160 m | 190 m | 320 m |
| 2 | 255 m | 295 m | 435 m |
| 3 | 500 m | 560 m | 620 m |

(`cell_active_dist_sq`, `cell_preload_dist_sq`: `lcs_world_hd.cpp:87-95`; evicção em
`step_streaming`.) Com raio 3 o cruzeiro mostra ~62..71 células em cache, ~35..37 prontas,
2.300..2.500 instâncias combinadas.

## 3. Sincronização entre threads

Os crashes antigos vinham de corridas entre RenderThread, gGameTickThread e cUmdThread. As
proteções em vigor (`lcs_world_hd.cpp` ~330-340):

- `ws_mtx` (recursivo) — serializa `Render` contra `Update`/`LoadLevel`/`LoadScene`/
  `RemoveOldSectorModels`/`SwapToStreamedSector`; travado no `pre` e liberado no `post`.
- `mtx` (recursivo) — estado interno das células.
- `stream_mtx` + `stream_cv` — `StreamingCallback` (cUmdThread) contra `Update`/`Stream`/
  `CancelStreaming`; cede a trava durante `PollStreaming` para evitar deadlock.
- `tex_mgr_mtx` — o `std::map` do `lglTextureManager` não tinha trava no jogo original.
- `render_depth` (atômico) — adia evicção enquanto `Render` lê payloads.

Com a GameTickThread desativada (ver [PERFORMANCE.md §4](PERFORMANCE.md)), a maior parte
dessas travas fica sem contenção, mas **devem permanecer**: o streaming assíncrono continua em
outra thread, e qualquer retorno a um modelo com duas threads volta a depender delas.

## 4. O que o `LcsDistance` faz

Hooks e patches (constantes em `lcs_distance.cpp:75-91`):

- `CPools::Initialise` — amplia pools para mais entidades/veículos/peds.
- `C_DeviceDetailSettings::GetLODSettings`, `GetLODSettingsLevel`, `GetCullFadeLevel`,
  `CSimpleModelInfo::GetElementFromDistance` / maior LOD — escala da distância de LOD
  (`world.detail_distance`).
- `CCamera::Process` + `RslCameraSetFarClipPlane` — far clip escalado (até 3.200 m).
- `CRenderer::ScanWorld` / `RequestObjectsInFrustum` — limites de 300 m e o *split* de
  prioridade escalados até 3.200 m; clamp móvel de 330 m em `SetupBigBuildingVisibility`.
- `cWorldStream::AddDynamic` — distância de props dinâmicos 100 m → até 500 m.
- `CRenderer::SetupBigBuildingVisibility` / `on_scan_world_post` — esconde LOD gêmeo de áreas
  cobertas por HD (`is_entity_lod_covered_by_hd`, linha ~366).
- `CPopulation::ManagePopulation` — raio de conversão 80 m → até 180 m.
- `CCarCtrl::GenerateOneRandomCar` — distâncias mínimas de spawn elevadas
  (55→100, 140→185, 40→105/115 m; `lcs_distance.cpp:1750-1759`) e
  `RecordNodeIndexesInCircle` em 360°.
- Neblina: `adjust_fog_parameters` recalcula início/fim a partir do far clip *base* do
  TimeCycle (senão a neblina some com far clip de 3.200 m).
- `requestEnableGameTickThread` desativado (thread única; ver PERFORMANCE.md §4).

## 5. Custos conhecidos (audit4, build quick)

| Ponto | Custo | Ideia |
|---|---|---|
| `step_streaming` ao carregar várias células | 68..79 ms num quadro | `fread` + inflate + validação em worker; `Fixup` nativo |
| `rebuild_combined_chunk` | 2,3..6,8 ms a cada mudança | Atualização incremental por célula |
| `prestream_sector_props` | ~0,01..0,19 ms | ok |
| `sync_prop_lod_distances` | < 0,07 ms | ok |

## 6. Problemas abertos

- **Carros presos em Shoreside:** a auditoria `audit3` encontrou 31 carros a 539..2.129 m
  que nunca são removidos. `CCarCtrl::PossiblyRemoveVehicle` (0x3b4eb0) só remove se
  `dist > s9 × [x23+0xf4] × 190` **e** o carro não estiver na tela (`GetIsOnScreen`,
  0x3b5210). Com frustum de 3.200 m quase tudo está na tela. Também pula se
  `CVehicle::CanBeDeleted` (0x4f7218) retornar 0 (`m_nCreatedBy` em `veh+0x2cc` igual a 2/4,
  flags `+0x291`, `+0x293`, peds não aleatórios dentro, trava bit3 `+0x2cd`). Correção
  proposta: forçar remoção de carros aleatórios/não-missão além de ~380 m mesmo "na tela".
  A sonda `[TMP_CAR_STUCK]` em `main.cpp:3169` lista esses carros.
- **Confirmar em jogo** com o usuário que a sobreposição LOD + HD e os spawns próximos
  sumiram (a auditoria automática deu zero, mas o usuário relatou ambos depois de correções
  anteriores).
- **Troca de ilha:** o HD World descarta tudo ao mudar de nível (`flush_all_cells`) e recomeça;
  somado à carga síncrona do jogo, gera o maior hitch (ver PERFORMANCE.md §3.3).
