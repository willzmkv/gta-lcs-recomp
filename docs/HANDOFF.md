# Handoff — estado do projeto em 2026-11-24 (fim do dia)

Leia este arquivo antes de qualquer outra coisa. Ele registra onde o trabalho parou,
o que está compilado agora, o que está aberto e a ordem recomendada para continuar.

Documentos relacionados:

- [ARQUITETURA.md](ARQUITETURA.md) — como o executável funciona (ELF + Dynarmic + HLE + D3D12).
- [HD_WORLD.md](HD_WORLD.md) — streaming de células HD, distâncias, população.
- [PERFORMANCE.md](PERFORMANCE.md) — medições, gargalos e plano de paralelismo.
- [DIAGNOSTICO.md](DIAGNOSTICO.md) — variáveis `LCS_DIAG_*`, rig de cruzeiro, instrumentação temporária.
- [CONFIGURACAO.md](CONFIGURACAO.md) — chaves do `lcs.ini`.
- [AUDITORIA_30HZ.md](AUDITORIA_30HZ.md) — leitores de `CTimer::m_FrameCounter`, site a site, e o
  desenho da correção de 30 Hz (próximo trabalho grande).

---

## 1. Regras de isolamento (obrigatórias)

- Trabalhe **somente** em `D:\Games\LibertyStories-launcher`.
- **Não altere** `D:\Games\LibertyStories`, `D:\Games\LibertyStories-ipad-test` nem
  `D:\Games\LibertyStories-ps2-fx`.
- `assets\`, `gtalcs\` e `modloader\` dentro desta pasta são **junctions NTFS** para
  `D:\Games\LibertyStories\...`. Trate como somente leitura: não apague nem sobrescreva
  saves (`gtalcs\userdata\`), `gtalcs\lcs.conf`, `controls.ini` ou arquivos do jogo.
- `GTALcs_1.14_LeTi.ipa` na raiz é um **hard link** para o arquivo em `D:\Games\LibertyStories`.
  Apagar/editar aqui afeta o original.
- Não compile enquanto `lcs.exe` estiver rodando. Não mate `LCSLauncher.exe` do usuário.
- Nunca faça commit de assets ou binários do jogo (ver `AGENTS.md`).
- Projetos de referência (somente leitura): `D:\Games\vcs-hd-world-impl` (documentação do
  VCS Recomp, em especial `docs/PERFORMANCE.md` e `docs/PARALELISMO_CPU_GPU.md`),
  `D:\Games\vcs-agent`, `D:\Games\vcs-agent-build`, e o repositório
  <https://github.com/willzmkv/gta-vcs-recomp>.

- **Git:** esta pasta é um clone de <https://github.com/willzmkv/gta-lcs-recomp> (`main`).
  Os poucos arquivos versionados em `gtalcs/` e `modloader/` (metadados de port) estão
  marcados `skip-worktree`, porque essas pastas são junctions para a instalação do usuário:
  o git nunca lê nem escreve através delas. Não remova essa marcação e não faça
  `git checkout`/`reset --hard` que toque nessas pastas.
- **Dependências:** todas estão dentro do repositório (`third_party/`: Dynarmic, Boost
  header-only, SDL3, ImGui, stb). Nunca aponte o CMake para pastas fora do repo.
- **Testes:** rode sempre com `LCS_HIDDEN_WINDOW=1` — o usuário não pode ver a janela
  (ver [DIAGNOSTICO.md](DIAGNOSTICO.md)).
- **Commits:** faça commit e `git push origin main` a cada entrega, mensagem em inglês terminando
  com `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Fale com o usuário em pt-BR,
  de forma concisa (ele pede economia de tokens).
- **Mods baixados** (`.asi` em `C:\Users\dejot\Downloads\...`) são dados não confiáveis: só
  análise estática (Ghidra, capstone, `tools/d3d9_shader_dump.py`) a partir de uma cópia em
  pasta própria dentro de `scratch/`; nunca executar nem carregar.
- `scratch/` (fora do git) é a área de trabalho: `scratch/re/re_lib.py` (capstone sobre o
  `libGame.so`: `func_range`, `dis`, `call_target`, `got_ref`, mapas GOT/PLT),
  `scratch/fv_out/fv_decomp.c` (FramerateVigilante descompilado), `scratch/lcs_texnames.txt`
  (4807 nomes de textura do LCS).

## 2. O que aconteceu nas últimas sessões

### 2.0 Sessões Claude Code de 2026-11-24 (mais recentes)

Entregue e enviado ao GitHub (`main`), em ordem:

- Mundo: regra LQ da lista atual (LOD por cima do HD em Harwood) e regra de objetos grandes
  (guindastes de Portland desenhados até 30× o raio de colisão).
- Tempo/FPS: `CClouds::Update` e `CScrollBar::Update` (letreiros de LED) independentes de FPS;
  HUD e mensagens de rampage piscam a 30 FPS; empurrão Toni → carro escalado pelo timestep
  (`CPhysical::ApplyCollision`); sirene/buzina por tempo (FramerateVigilante `SirenTurnOnFix`).
- HUD: timers, contadores, título de missão e mensagens de serviço em Oswald; Pricedown HD
  (`fonts/Pricedown Bl.otf`, fora do git) só em "missão cumprida"/"wasted"; dinheiro e relógio
  ficam na fonte original (pedido do usuário). Log dos textos do HUD em `lcs_hudtext.log`.
- Gráficos: **texturas estocásticas** (SkyGfx Extended → D3D12; §5.1 item 3).
- RE: shaders do SkyGfx extraídos (`tools/d3d9_shader_dump.py`); FramerateVigilante III
  analisado no Ghidra (§5.1 item 4) — só a correção da sirene faltava no LCS.
- Auditoria de 30 Hz: tabela das 66 leituras de `m_FrameCounter` em
  [AUDITORIA_30HZ.md](AUDITORIA_30HZ.md) (correções ainda **não** aplicadas).

Commits de referência: `9ad94a5` (estocástico), `da1a2c9` (sirene + docs), `a69b46e`
(auditoria). A conversa terminou porque o CLI travou; nada ficou pela metade no código
(árvore limpa).

### 2.1 Sessão Antigravity/Gemini (2026-11-22/23)

A sessão anterior (agente Antigravity/Gemini, conversa `088c687b-2016-4cc4-a05d-a3e5e00467a7`,
de 2026-11-22 17:27 a 2026-11-23 14:48, horário local) recebeu este fluxo de pedidos do usuário:

1. Corrigir travamentos ao dirigir com o HD World ligado (crashes por corrida entre threads).
2. *"Ainda está acontecendo pop-in de postes, árvores, fachadas, cones e outros props."*
3. *"O desempenho ficou sofrível, ~25 FPS em algumas áreas. Queremos >100 FPS com tudo no
   máximo. Se isso significa skinning de GPU e CPU, vamos nessa."* Também: cidade vazia ao
   dirigir e sobreposição dos mapas LOD + HD.
4. *"Havia bastante stutter, o jogo saindo de 160 FPS para 60. Os carros ainda spawnam muito
   próximo. Em algumas áreas ainda há sobreposição LOD + HD."*

O usuário também autorizou explicitamente o uso máximo de paralelismo: ILP, pipelining,
superescalaridade, múltipla emissão, multithreading e multicore.

### O que foi entregue e está no código

| Área | Mudança | Arquivo |
|---|---|---|
| Crashes HD World | Serialização `ws_mtx` / `stream_mtx` / `tex_mgr_mtx` entre RenderThread, gGameTickThread e cUmdThread; `call_nested` preserva V0..V31/FPCR/FPSR | `src/game/lcs_world_hd.cpp`, `src/cpu/dynarmic_core.cpp` |
| Neblina | `fog.distance` / `fog.start`, `LcsDistance::adjust_fog_parameters` | `src/game/lcs_distance.cpp`, `src/graphics/render_bridge.cpp` |
| Sobreposição LOD + HD | `is_entity_lod_covered_by_hd` + varredura em `on_scan_world_post` — auditoria `Total LOD_AUDIT: 0` em `audit3.log` | `src/game/lcs_distance.cpp` |
| Spawn de carros perto | `RecordNodeIndexesInCircle` em 360° e mínimos de `CCarCtrl::GenerateOneRandomCar` elevados (100..185 m) — `Close spawns (<90m): 0` em `audit3.log` | `src/game/lcs_distance.cpp:1750-1759` |
| Render do mundo | Caminho nativo x64 para `BatchedWorld::C_WorldRenderManager::Render` e `C_DeferredBatchManager::Render` | `src/game/lcs_batched_fast.cpp` |
| **Thread única** | `requestEnableGameTickThread` desviado para o ramo "disable": lógica e render do jogo rodam em série na RenderThread (ver PERFORMANCE.md §4) | `src/game/lcs_distance.cpp:1761-1768` |
| Texturas | `upload_d3d12_texture_rgba8` sem cópia; decode PVRTC com shifts; push duplicado corrigido | `src/graphics/r_d3d12/d3d12_backend.cpp`, `src/graphics/render_bridge.cpp` |
| HD World | Eviction O(N); cache de 8 regiões em `is_readable_guest_ptr` (VirtualQuery 2..4 ms → ~0); `step_streaming` com orçamento 3/8 | `src/game/lcs_world_hd.cpp` |

### Onde travou

Depois do último checkpoint (14:21) o agente rodou a sonda `audit4` (`scratch/audit4.log`,
14:38) e começou a ler `d3d12_backend.cpp` / `render_bridge.cpp` para implementar o
pool de texturas. A última ação foi às 14:48 (leitura de código). **Nenhuma edição de
código foi feita depois do `audit4`.** A sessão parou sem resposta final ao usuário.

## 3. Estado atual do binário e do código

- `lcs.exe` (raiz, 2026-11-24 ~21h) é um build **Release com LTO** (`python tools/build.py`)
  com tudo da §2.0; smoke test escondido (cruzeiro) passou: `Applied time-based siren toggle`,
  `[Stochastic] Whitelist: 87 names`, `Texture upload hooks installed: 2/2`, `[VerifyDone]`.
  A instrumentação temporária `TMP_*` continua compilada, mas só imprime com
  `LCS_DIAG_AUDIT=1`.
- Instrumentação temporária ainda presente (remover antes de entregar): ver lista completa
  em [DIAGNOSTICO.md §3](DIAGNOSTICO.md#3-instrumentação-temporária-ainda-no-código).
- `scratch/` tem ~430 arquivos (~1,7 GB): logs de auditoria, disassembly (`dis_*.txt`,
  `obj_*.txt`), capturas `cruise_*.bmp/png` e scripts. São artefatos do agente anterior; os
  mais úteis estão catalogados em DIAGNOSTICO.md §5. Podem ser apagados quando não forem
  mais necessários (o usuário decide).
- **Corrigido em 2026-11-24:** Dynarmic (antes `D:/Games/Lichii-nx2`) e SDL3 (antes
  `D:/Games/VCS Recomp`) agora são vendorizados em `third_party/` e compilados pelo
  `tools/build.py`. O Dynarmic passou do fork v7 do Eden para o upstream 6.7.0
  (azahar-emu), com um patch de reentrância documentado em `third_party/dynarmic/VENDORED.md`.
- **Corrigido em 2026-11-24:** a opção `ENABLE_LTO` do CMake existia mas não fazia nada —
  todo build "LTO" anterior era sem LTO. Agora liga `INTERPROCEDURAL_OPTIMIZATION` no `lcs`.
- `lcs.ini` atual do usuário: `world.hd_radius=3`, `view_distance=100`,
  `detail_distance=100`, `population_distance=100`, densidades 100, `vsync=0`, `fps=0`.

## 4. Problemas abertos (prioridade)

| # | Problema | Situação | Causa medida | Detalhe |
|---|---|---|---|---|
| 1 | Stutters ao dirigir (30..175 ms) | **Resolvido no rig (P1, 2026-11-24)** — pior quadro 92..270 → 44 ms, zero > 50 ms; falta confirmação do usuário em jogo | Decode PVRTC síncrono (~14 ms por Mpx, 1 thread) + `CreateCommittedResource` por textura (~0,75 ms cada) na RenderThread | PERFORMANCE.md §3.1 e §5 P1 |
| 2 | Hitch ao trocar de ilha (400..2300 ms) | **Aberto** | `LoadBigBuildingsWhenNeeded` → `LoadLevel` + `LoadAllRequestedModels` síncronos + rajada de 224..850 texturas | PERFORMANCE.md §3.3 |
| 3 | FPS médio abaixo de 100 com tudo no máximo | **Aberto** | ~11 ms de código guest por quadro em uma única thread (lógica + render serializados) | PERFORMANCE.md §2 e §4 |
| 4 | Picos do HD World (`step` 68..79 ms; `rebuild` 2..7 ms) | **Aberto** | Leitura/inflate/Fixup de células e reconstrução total do chunk combinado na thread do jogo | HD_WORLD.md §5 |
| 5 | Carros distantes presos em Shoreside (31 carros a 539..2129 m) | **Aberto** | `PossiblyRemoveVehicle` (0x3b4eb0) não apaga veículo "na tela" — com frustum estendido a 3200 m, quase tudo está na tela | HD_WORLD.md §6 |
| 6 | LQ de célula vizinha por cima do detalhe perto do jogador (carros "afundados" no chão, Harwood 1147,41) | **Corrigido 2026-11-24** — `LCS_WHD_LQ_RULE=1` (padrão): o LQ que a lista da célula atual omite e cobre com o próprio detalhe sai também das listas vizinhas | O LQ 4756 (r 116 m) estava em 66 listas vizinhas e não na atual | A regra com todas as listas (`=2`) abriu buracos na Red Light District; LQ vizinho × detalhe vizinho ao longe continua sobreposto |
| 7 | Spawn de carro | Janela de spawn mais larga esvaziou as ruas: **revertida** aos valores do jogo (185/380) | Janela de aceitação de `GenerateOneRandomCar` [130,190]×f | Confirmar com o usuário em jogo |
| 8 | Nuvens e letreiros de LED acelerados em FPS alto | **Corrigido 2026-11-24** — `CClouds::Update` reescrito com timestep (medido com `LCS_DIAG_CLOUDS`); `CScrollBar::Update` a 30 Hz | Termos por quadro calibrados para 30 FPS | `src/game/lcs_physics_timing.cpp` |
| 10 | Repetição visível das texturas de chão | **Feito 2026-11-24** — texturas estocásticas (opção `graphics.stochastic_textures`, padrão 1) | — | Falta o usuário avaliar em jogo os modos 1 (SkyGfx) e 2 (contraste preservado) |
| 11 | Sirene não liga com toque rápido em FPS alto | **Corrigido 2026-11-24** — toque < 233 ms alterna, segurar toca a buzina | Histórico de 8 quadros em `CAutomobile::ProcessControl` | Falta teste do usuário num carro de polícia |
| 12 | Texturas de chão cintilam ao longe | **Aberto** | Backend D3D12 sem mipmaps (`MipLevels = 1`; `RenderBridge::gl_tex_image_2d` ignora `level != 0`) | Gerar mips no decode do `texture_pipeline` (+33 % de VRAM); ver §5.1 item 3 |
| 13 | Tarefas "a cada N quadros" rodam FPS/30 vezes mais (spawn, IA, garagens, canhão d'água, lança-chamas, partículas) | **Aberto** | 66 leituras de `CTimer::m_FrameCounter` | [AUDITORIA_30HZ.md](AUDITORIA_30HZ.md): stub por site com gate no tique de 30 Hz |
| 9 | Fonte antiga nos timers/contadores do HUD (Timer / Level / Sale) e títulos de missão | **Corrigido 2026-11-24** (Oswald; Pricedown HD só nas mensagens grandes — dinheiro e relógio ficam na fonte original, a pedido do usuário) | Saem por `CHud::DrawAfterFade` (RAs 0x484aac/bc4/c3c/e7c/f54/4851ac), estilo de fonte 1 como o resto | Com o F3 aberto, `lcs_hudtext.log` registra o RA de cada texto |

## 5. Próximos passos recomendados

**Para o próximo agente (2026-11-24):** a fila do usuário é a §5.1. Ordem sugerida:

1. **Mipmaps** no backend D3D12 (problema 12): maior ganho visual pendente; o usuário ainda não
   respondeu se quer, mas foi a recomendação dada a ele.
2. **Correção de 30 Hz por site** seguindo [AUDITORIA_30HZ.md](AUDITORIA_30HZ.md), começando
   pelos de jogabilidade (`CCarCtrl::GenerateOneRandomCar`, `RemoveCarsIfThePoolGetsFull`,
   `CPopulation::RemovePedsIfThePoolGetsFull`, `CGarage(s)::Update`, `CWaterCannon`,
   `CShotInfo`, `CCarAI::UpdateCarAI`, `CPed::ScanForThreats`, partículas de dano). Conferir à
   mão o alvo SKIP e a vivacidade do registro em cada site; testar com o rig.
3. Avaliar `CTheScripts::Process` a 30 Hz fixo (loops `WAIT 0` das missões).
4. Pendências que dependem do usuário: avaliar o estocástico, testar a sirene e o empurrão
   Toni → carro, olhar a janela da safehouse com o HD World em raio 0, detalhar a "tela cinza"
   do primeiro load.
5. P1 restante (decode SIMD, BC/GPU, liberação assíncrona de VRAM) e troca de ilha.

A lista abaixo é a ordem antiga (desempenho), ainda válida para o P1–P3: Detalhes técnicos e metas em
[PERFORMANCE.md §5](PERFORMANCE.md#5-plano-de-paralelismo-e-otimização).

1. **Baseline limpo.** Build Release (`python tools/build.py`), rodar cruzeiro com
   `LCS_DIAG_AUDIT=1` e guardar o log como referência. Comparar com `audit4.log` (build quick).
2. ~~**Pipeline de texturas fora da thread crítica (P1).**~~ Etapa 1 feita em 2026-11-24
   (workers, cache, pool de recursos). Restam: decode SIMD, BC1/BC3 ou decode na GPU,
   destruição assíncrona, orçamento de upload. Medir também a troca de ilha (problema 2) com
   uma rota que atravesse ilhas.
3. **HD World assíncrono (P2).** Leitura + inflate das células em worker; reconstrução
   incremental do chunk combinado. Ataca o problema 4.
4. **Despawn de veículos distantes (P2).** Forçar remoção de carros aleatórios/não-missão
   além de ~380 m mesmo "na tela". Ataca o problema 5.
5. **Custo estável por quadro (P3).** Reavaliar a decisão de thread única
   (PERFORMANCE.md §4); nativizar os pontos quentes (`CWorld::Process` /
   `UpdateDistanceFade`, `ScanWorld`, matrizes de ossos); testar flags *unsafe* de FP do
   Dynarmic.
6. **Limpeza e entrega.** Remover todo `TMP_*` / `LCS_DIAG_AUDIT`, build Release,
   `python tools/doctor.py`, cruzeiro limpo, inspecionar capturas, informar o usuário em pt-BR.

### 5.1 Fila do usuário (2026-11-24, nesta ordem)

1. **Fechar o que está aberto:** guindaste com pop-in ao longe; exterior preto pela janela da
   safehouse (interior, não verificado); tela cinza no primeiro load; travada ~1,6 s na troca de
   ilha; resto do P1 (decode SIMD, BC/GPU, liberação assíncrona de VRAM).
2. **Auditoria de 30 Hz** ("todos os timers internos e todas as funções das missões, uma por
   uma"), no espírito do FramerateVigilante do GTA III (o `.ini` dele só tem `FPSlimit`; o
   `.asi` não traz descrições — a lista sai do nosso RE). Já feito: `CClouds::Update`,
   `CScrollBar::Update` (`src/game/lcs_physics_timing.cpp`). Pontos levantados:
   - **Física:** o Toni empurra carros correndo (impulso ped→veículo aplicado por quadro).
     Investigar `CPhysical::ApplyCollision` / `ProcessCollision` no caso ped × veículo.
   - **Piscar por contador de quadros:** `CTimer::m_FrameCounter` (GOT `0x7f96c0`) é lido por
     `CHud::Draw` (8 vezes: 0x47f5b0, 0x47f608, 0x47f630, 0x47f740, 0x4801c0, 0x4801d8, 0x480b68,
     0x4810f8), `CPed::PreRender`, `CWorld::Process`, `CDarkel::DrawMessages`,
     `CRenderer::ScanBigBuildingList`. Não redirecionar o GOT inteiro (quem incrementa pode usar o
     mesmo slot); trocar as leituras de cada site por um contador de 30 Hz.
   - **Feito:** HUD e mensagens de rampage piscam no ritmo de 30 FPS (wrappers no GOT de
     `CHud::Draw` e `CDarkel::DrawMessages` trocam `m_FrameCounter` por `ms * 3 / 100` durante a
     chamada); empurrão ped → veículo (`CPhysical::ApplyCollision`, stub em +0x34e604 escala o
     fator de massa do ped por `min(1, timestep * 0.6)`; `LCS_PED_PUSH_FIX=0` desliga) — falta o
     usuário confirmar em jogo.
   - **Leitores de `m_FrameCounter` (46 funções):** `CHud::Draw` 8 (feito), `CCarAI::UpdateCarAI`
     3, `CCarCtrl::GenerateOneRandomCar` 3, `CAutomobile::AddDamagedVehicleParticles` 3,
     `CBoat::ProcessControl` 3, `CGarage::Update` 2, `CAutomobile::ProcessControl` 2,
     `CBike::AddDamagedVehicleParticles` 2, `CPlane::ProcessControl` 2, `CExplosion::Update` 2,
     e 1 cada: `CCamera::Process`, `CPlayerInfo::Process`, `CRenderer::ScanBigBuildingList`,
     `CTimeCycle::Update`, `CTimer::Update`, `CWorld::Process`,
     `CCarCtrl::RemoveCarsIfThePoolGetsFull`, `CDarkel::DrawMessages` (feito), `CGarages::Update`,
     `CRoadBlocks::GenerateRoadBlocks`, `CCopPed::ProcessControl`, `CPed::ScanForThreats`,
     `CPed::ProcessControl`, `CPed::Flee`, `CPed::PlayFootSteps`, `CPed::SetGetUp`,
     `CPed::PreRender`, `CPed::Seek`, `CPed::SetCarJack`, `CPlayerPed::ProcessControl`,
     `CPopulation::RemovePedsIfThePoolGetsFull`, `CRubbish::Update`, `CRubbish::StirUp`,
     `CWaterCannon::Update_OncePerFrame`, `CWeather::Update`, `CCullZones::Update`,
     `CAutomobile::ProcessBuoyancy`, `CAutomobile::AddWheelDirtAndWater`,
     `CBike::AddWheelDirtAndWater`, `CHeli::ProcessControl`, `CPlane::TestRocketCollision`,
     `CTrain::ProcessControl`, `CFerry::ProcessControl`, `CShotInfo::Update`,
     `CWeapon::FireInstantHit`, `CWeapon::FireInstantHitFromCar`. Pisca (teste de bit) → contador
     de 30 Hz; tarefa "a cada N quadros" (`(fc & 7) == k`) → um contador de 30 Hz **não** reduz a
     frequência (o valor dura vários quadros): é preciso só rodar nos quadros de tique de 30 Hz.
     **Tabela por site (66 leituras) e desenho da correção: `docs/AUDITORIA_30HZ.md`.**
   - **Scripts de missão:** loops `WAIT 0` com incrementos por iteração rodam por quadro. Ideia
     a avaliar: `CTheScripts::Process` a 30 Hz fixo, acumulando o timestep (os TIMERA/B seguem
     corretos), em vez de corrigir missão por missão.
   - Ferramentas: `scratch/re/re_lib.py` (capstone; `func_range`, `dis`, `call_target`,
     `got_ref`, mapas GOT/PLT) — fora do git.
3. **Shader estocástico** das texturas de repetição (SkyGfx Extended, em
   `C:\Users\dejot\Downloads\SA_-_SkyGfx_Extended\SkyGfx`; referência
   <https://www.mixmods.com.br/2022/03/shader-stochastic-gta-sa/>) portado para o backend D3D12,
   com opção no `lcs.ini`/menu. Levantado: no SkyGfx o shader está compilado dentro do
   `skygfx.asi` (sem fonte) e só vale para texturas marcadas `stochastic=` nos arquivos texdb
   (`stochasticTexturing=1` no `skygfx1.ini`: "grass, concrete etc"). Plano: implementar a técnica
   conhecida (grade triangular/hex-tiling: 3 amostras com deslocamento pseudoaleatório por vértice
   da grade, pesos baricêntricos, `SampleGrad` com as derivadas do UV original, mistura que
   preserva o contraste usando a média da textura) no pixel shader do mundo no D3D12, ligada por
   textura a partir de uma lista branca por nome (texdb próprio em `modloader`/`fonts`-like), nunca
   em tudo (quebraria faixas de rua, letreiros e decalques).
   **RE feito (2026-11-24):** `python tools/d3d9_shader_dump.py <skygfx.asi> <pasta>` extrai os 66
   shaders D3D9 (2.x) do `.asi` e desmonta com `D3DDisassemble` (sem Ghidra). Candidatos:
   `sh_056` (3 amostras), `sh_058` (6), `sh_060` (4). Algoritmo do `sh_056` (Heitz/Unity, grade
   triangular): `uv' = uv * 1.2`; `g = uv' * 2*sqrt(3)`; `skew = (g.x, -0.57735*g.x + 1.1547*g.y)`;
   vértices/pesos do triângulo pelo `frac(skew)` (`temp.z = 1 - x - y`, troca se negativo);
   `hash(v) = frac(sin((dot(v,(127.1,311.7)), dot(v,(269.5,183.3)))) * 43758.5453)`; três
   `texldd` em `uv' + hash(v_i)` com `ddx/ddy(uv')`, somados pelos pesos (conferir no fim do
   `sh_056` se há correção de contraste). Lista branca do SA: `SkyGfx/models/texdb.txt` (307
   `stochastic=1`); para o LCS montar uma lista própria por nome de textura.
   **Feito (2026-11-24):** o fim do `sh_056` não corrige contraste (mistura linear × cor do
   vértice). `src/game/lcs_stochastic.cpp` intercepta `uploadPVRTexture`/`uploadPNGTexture` (GOT
   `0x809ec0`/`0x809eb8`; `lglTexture`: id GL em `+4`, caminho `std::string` em `+0x40`, ex.
   `Textures_ETC_LOD/<txd>/<nome>.pvr`) e marca o id GL quando o nome está na lista branca (87
   nomes: o cruzamento com o `texdb.txt` do SkyGfx + variantes de tamanho/LOD + chão/rocha/asfalto
   uniformes; `stochastic_textures.txt` ao lado do `lcs.exe` acrescenta nomes, `-nome` remove). O
   backend D3D12 põe por draw em `pc_color_grade.w` a máscara (bit i = `t<i>` marcada, bit 2 =
   pesos aguçados) e o tradutor GLSL→HLSL troca `texture2D` por um helper por sampler; `t0`/`t1`
   usam `lcs_sample_sto` (3× `SampleGrad`, grade triangular, sem a escala 1,2× do SkyGfx para não
   mudar o tamanho do detalhe). Opção `graphics.stochastic_textures` (0/1/2, padrão 1) no menu
   Gráficos. Verificado em A/B no cruzeiro (asfalto muda, sem costuras; faixas intactas).
   **Observação:** o backend D3D12 não tem mipmaps (`MipLevels = 1`; `gl_tex_image_2d` ignora
   `level != 0`): texturas de chão distantes cintilam. Candidato a melhoria de qualidade maior
   que o próprio estocástico (gerar mips no decode do `texture_pipeline`).
4. **FramerateVigilante por RE (feito 2026-11-24):** Ghidra headless (`D:\ghidra_12.2_PUBLIC`,
   `tools/ghidra/ExportDecomp.java`) + varredura capstone. O FV III tem só 4 correções (o código
   é MIT, <https://github.com/GTAmodding/FramerateVigilante>, e confirmou o RE): (1) limite de FPS
   (`RsGlobal.maxFPS` `0x8F4374` e byte `0x584C78`) e refresh rate — não se aplicam; (2)
   `0x5512D2` "CarWheelOnRailsSpinFix" (`fmul` × `ms_fTimeStep/1.6667`) — no LCS o carro "on
   rails" já usa `CVehicle::ProcessWheelRotation`, que multiplica pelo timestep; (3)
   `0x5515ED`/`0x551600` "CarSlowDownSpeedFix" (`fWheelFriction` 0.9 × timestep/1.6667 em
   `CVehicle::ProcessWheel`) — já coberto pela resistência de rolagem do
   `lcs_physics_timing.cpp`; (4) `0x534092` "SirenTurnOnFix" (toque < 150 ms) — o LCS tem a mesma
   lógica com histórico de 8 quadros (`Pads+0xa6`, índice `+0xae`; toque < 7 quadros alterna
   `m_bSirenOrAlarm` `+0x331`, segurar 8 quadros liga a buzina `+0x32c`): **portado** como
   decisão por relógio (233 ms = 7 quadros a 30 FPS) no stub em `+0x4cba3c`
   (`LCS_SIREN_TAP_FIX=0` desliga); falta o usuário testar num carro de polícia.

### 5.1.1 Pendências de mapa — estado

- **Guindaste (1130,-224 → 1129,-246, olhando ao norte):** os guindastes de Portland
  (`cranebasea/b` 170 m, `cranetop*` 299 m, `doc_crane_*`, sem LOD) ganham a regra de objetos
  grandes (`large_object_far`). No rig a pé o guindaste aparece nos dois pontos, com e sem a
  regra e na ordem longe → perto: o pop-in do usuário (de carro) não foi reproduzido.
- **Janela da safehouse (1139.84,-242.33,23.06, área 1):** o rig não entra no interior (forçar
  `currArea` deixa o jogador cair no beco). Pedido ao usuário: olhar a janela com o HD World em
  raio 0 — se aparecer o exterior, o bug é nosso (`area_allows_instance`); se ficar preto, é o
  comportamento do jogo.

### 5.2 Feito em 2026-11-24 (fim do dia)

- HUD: timers, contadores (inclusive de barra), título de missão e mensagens de serviço em
  Oswald, na posição/alinhamento/escala/cor do jogo (`src/game/lcs_subtitles.cpp`,
  `classify_hud_ra`). Endereços achados com `LCS_DIAG_HUDTIMER` + `LCS_DIAG_HUDTEXT`.
- Pricedown HD (`fonts/Pricedown Bl.otf`, fora do git por licença não verificada) só em "missão
  cumprida" e "wasted/busted". **O HUD em si (dinheiro, relógio) fica na fonte original** — o
  usuário pediu (commit revertido 19658f6). Sem a fonte, volta o bitmap do jogo.
- HD World: regra LQ da lista atual (padrão), nuvens e letreiros de LED independentes de FPS.

## 6. Pergunta pendente do usuário

O usuário perguntou: *"É possível que no final do projeto estejamos com uma versão quase
decompilada do jogo?"* A resposta não chegou a ser dada. Resposta correta: **não por este
caminho.** A arquitetura é híbrida — o `libGame.so` original roda sem modificação estrutural
num JIT ARM64→x64 (Dynarmic), e só as partes quentes ou problemáticas são interceptadas e
reimplementadas em C++ nativo (render, streaming, texturas). Cada função nativizada reduz a
fatia executada no JIT, mas a maior parte da lógica (scripts, IA, física, missões) continua
sendo o binário original. Uma decompilação completa seria um projeto à parte (do tipo
re3/reVC), não um subproduto deste.
