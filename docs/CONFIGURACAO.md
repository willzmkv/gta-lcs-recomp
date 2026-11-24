# Configuração — `lcs.ini`

`lcs.ini` fica na raiz, ao lado de `lcs.exe`, e é lido/escrito tanto pelo `LCSLauncher.exe`
quanto pelo menu de configurações do jogo (F2). Código: `src/settings/settings.cpp`
(leitura nas linhas ~65-121, escrita ~129+). Valores fora da faixa são limitados (`clamp`).

O `launcher.ini` guarda só as preferências do próprio launcher (`after_launch`, `theme`,
`reduce_motion`, …). `gtalcs/lcs.conf` e `gtalcs/controls.ini` são do jogo e **não** devem
ser sobrescritos.

## Tela

| Chave | Faixa | Significado |
|---|---|---|
| `display.window` | 0..1 | 0 = tela cheia sem bordas, 1 = janela |
| `display.aspect` | 0/169/219/43 | Auto, 16:9, 21:9, 4:3 |
| `display.hud` | 0..2 | Esticado, 4:3 centralizado, widescreen |
| `display.fps` | 0..500 | Limite de FPS (0 = sem limite / V-Sync) |
| `display.vsync` | 0..1 | V-Sync |
| `display.fps_counter` | 0..2 | Desligado, simples, detalhado |
| `display.renderer` | 0..2 | 0 = Direct3D 12, 1 = Vulkan (backend Vulkan ainda é esqueleto) |
| `display.scale` | 0..8 | Escala interna (0 = igual à janela) |
| `display.msaa` | 1..8 | MSAA |
| `display.aniso` | 0..16 | Filtro anisotrópico |
| `display.skip_intro` | 0..1 | Pular vídeos de abertura |

## Gráficos

| Chave | Faixa | Significado |
|---|---|---|
| `graphics.ipad_shadows` | 0..1 | Sombras dinâmicas do iPad |
| `graphics.color_grading` (alias `graphics.post_process`) | 0..1 | Color grading Retina do iPad |
| `graphics.brightness` / `contrast` / `saturation` | 0..100 | Ajustes de imagem |
| `graphics.texture_replacements` | 0..1 | Texturas de `modloader/` |
| `ps2.pipeline` | 0..1 | Liga o pipeline de efeitos do PS2 |
| `ps2.timecycle` | 0..2 | 0 = iPad/Recomp, 1 = PS2 original, 2 = híbrido |
| `ps2.radiosity`, `ps2.color_filter`, `ps2.trails`, `ps2.speed_blur` | 0..1 | Efeitos individuais |
| `ps2.*_intensity` | 0..200 | Intensidade (%) |

## Mundo (HD World e distâncias — ver [HD_WORLD.md](HD_WORLD.md))

| Chave | Faixa | Significado |
|---|---|---|
| `world.hd_radius` | 0..3 | 0 = original; 1..3 = células HD extras (3 ≈ 500 m) |
| `world.view_distance` | 0..100 | Far clip (até 3.200 m) |
| `world.detail_distance` | 0..100 | Distância de troca de LOD e de props |
| `world.population_distance` | 0..100 | Raio de spawn/remoção de peds e carros |
| `world.traffic_density` (alias `vehicle_density`) | 0..100 | Densidade de trânsito |
| `world.ped_density` (alias `pedestrian_density`) | 0..100 | Densidade de pedestres |
| `world.streaming_memory` | 64..512 | MiB retidos pelo streaming |
| `fog.distance` | 10..100 | Fim da neblina (% do far clip base) |
| `fog.start` | 0..300 | Início da neblina (%) |

## Áudio, controles, legendas

| Chave | Faixa | Significado |
|---|---|---|
| `audio.volume`, `audio.music_volume` (alias `movie_volume`), `audio.sfx_volume` | 0..100 | Volumes |
| `mouse.free_aim` | 0..1 | Mira livre 1:1 |
| `mouse.sensitivity` | 10..400 | % |
| `mouse.invert_y` | 0..1 | |
| `pad.vibration` | 0..200 | % |
| `enhance.prompts` | 1..4 | Ícones: auto, PlayStation, Xbox, teclado |
| `enhance.free_cam_speed` | 25..400 | % |
| `enhance.subtitle_font` | 0..1 | Original / Oswald |
| `enhance.subtitle_outline`, `_size`, `_shadow`, `_background`, `_colour` | — | Estilo de legenda |
| `ui.font_scale` | 90..140 | Escala da fonte dos menus |
| `game.language` | 0..7 | EN, FR, DE, IT, ES, RU, JA, PT-BR |
| `mods.enabled` | 0..1 | `modloader/` |

## Chaves que o `lcs.exe` ignora

O launcher (código herdado do VCS em `launcher/model/game_config.cpp`) ainda grava chaves que
o jogo **não lê**: `world.crowd_bands`, `world.fps_target`, `world.distant_traffic`,
`world.distant_crowds`, `display.timing`, `display.cutscene_bars`, `graphics.exposure`,
`memory.streaming`, `enhance.prompt_style`, `enhance.driveby_rate`, `pad.preset`,
`pad.trigger_*`, `pad.free_aim`, `pad.aim_assist`, `pad.deadzone_*`, `pad.camera_speed`,
`pad.invert_y`, `pad.ds_*`, `mouse.camera`, `mouse.wheel`, `audio.output`,
`game.background`, `ipa`. Elas não têm efeito até alguém implementá-las ou removê-las do
launcher.
