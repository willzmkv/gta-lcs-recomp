# Auditoria de 30 Hz — leitores de `CTimer::m_FrameCounter`

Levantamento de 2026-11-24 (66 leituras em 46 funções do `libGame.so`). O jogo foi feito para
30 FPS: tarefas "a cada N quadros" (`(fc & M) == K`, `(fc + id) & M`, bit de paridade) rodam
`FPS/30` vezes mais a 144 FPS — spawn e remoção de carros/pedestres, IA, garagens, empurrão do
canhão d'água, fogo do lança-chamas, partículas.

## Por que não basta um contador de 30 Hz

Trocar `fc` por `T = ms * 3 / 100` não reduz a frequência: o valor dura `FPS/30` quadros e a
condição continua verdadeira na mesma fração dos quadros. Gatear só a função chamada (ex.
`CWaterCannon::PushPeds` apenas em quadros de tique) cria *aliasing* em 120/144 Hz: os quadros
de tique caem sempre na mesma fase de `fc & 3` (nunca ou sempre coincidem).

## Correção proposta (por site)

Trocar a instrução que lê o contador (`ldr/ldrb wN, [xM]`) por `b stub` (stub alocado a menos de
128 MB do código, perto do fim da imagem). O stub carrega uma palavra com `T` nos bits 0–30 e o
bit 31 = "este quadro é um tique de 30 Hz" (atualizada uma vez por quadro em
`tick_fixed_rate_effects`):

- **periódico** (trabalho que muda estado ou emite partículas): fora do tique desvia para o
  caminho "sem trabalho" (SKIP, conferido à mão em cada site, incluindo se `wN` está morto lá);
  no tique segue com `wN = T` (`& 0xff` nos sites `ldrb`). Resultado: a condição avalia cada
  valor de `T` exatamente uma vez → frequência de 30 FPS, sem aliasing.
- **visual por quadro** (luz/corona registrada todo quadro, pisca-pisca): só `wN = T`, sem
  gate — gatear faria o efeito sumir nos quadros sem tique.

O stub usa apenas `xN` (destino da leitura, que seria sobrescrito de qualquer forma).

## Sites

| Endereço | Função | Padrão (após a leitura) | Classe |
|---|---|---|---|
| `0x329504` | `_ZN7CCamera7ProcessEv` | `ldr w8, [x8]; and w8, w8, #0xf; cmp w8, #3; b.ne #0x329524` | periódico → gate no tique + contador 30 Hz |
| `0x3531fc` | `_ZN11CPlayerInfo7ProcessEv` | `ldrb w8, [x27]; tst w8, #0xf; b.eq #0x353214; ldr s0, [x19, #0x170]` | periódico → gate no tique + contador 30 Hz |
| `0x360a6c` | `_ZN9CRenderer19ScanBigBuildingListER8CPtrList` | `ldr x21, [x21, #0x208]; ldr x22, [x22, #0x210]; ldr w8, [x8]; ldr x23, [x23, #0x220]; ldr ` | render: distribui varredura → manter |
| `0x3705f4` | `_ZN10CTimeCycle6UpdateEv` | `ldrb w8, [x8]; tst w8, #0xf; b.eq #0x370608` | periódico → gate no tique + contador 30 Hz |
| `0x370cdc` | `_ZN6CTimer6UpdateEv` | `ldr w9, [x8]; ldr x21, [sp, #0x10]; add w9, w9, #1` | incremento — não mexer |
| `0x377d38` | `_ZN6CWorld7ProcessEv` | `ldrb w8, [x8]; tst w8, #0x3f; b.ne #0x377d4c` | periódico → gate no tique + contador 30 Hz |
| `0x3b0588` | `_ZN6CCarAI11UpdateCarAIEP8CVehicle` | `ldrh w9, [x19, #0x7a]; ldr w8, [x8]; and w9, w9, #0x3f; and w8, w8, #0x3f; cmp w8, w9` | periódico → gate no tique + contador 30 Hz |
| `0x3b0a4c` | `_ZN6CCarAI11UpdateCarAIEP8CVehicle` | `ldrb w9, [x9]; tst w9, #7; b.ne #0x3b1120; ldr d2, [x8]; add x1, sp, #0x50` | periódico → gate no tique + contador 30 Hz |
| `0x3b1720` | `_ZN6CCarAI11UpdateCarAIEP8CVehicle` | `ldrb w9, [x9]; tst w9, #7; b.ne #0x3b1120; ldr d2, [x8]; add x1, sp, #0x40` | periódico → gate no tique + contador 30 Hz |
| `0x3b29f4` | `_ZN8CCarCtrl20GenerateOneRandomCarEv` | `ldr w8, [x8]; and w8, w8, #3; cmp w8, #2; b.hs #0x3b2aa4` | periódico → gate no tique + contador 30 Hz |
| `0x3b2a7c` | `_ZN8CCarCtrl20GenerateOneRandomCarEv` | `ldr w8, [x8]; and w8, w8, #3; sub w9, w8, #2; cmp w9, #2` | periódico → gate no tique + contador 30 Hz |
| `0x3b2adc` | `_ZN8CCarCtrl20GenerateOneRandomCarEv` | `ldrb w8, [x8]; tbnz w8, #0, #0x3b2b1c` | periódico → gate no tique + contador 30 Hz |
| `0x3b5300` | `_ZN8CCarCtrl27RemoveCarsIfThePoolGetsFullEv` | `ldr w8, [x8]; and w8, w8, #7; cmp w8, #2; b.ne #0x3b53e0` | periódico → gate no tique + contador 30 Hz |
| `0x3bdfec` | `_ZN7CDarkel12DrawMessagesEv` | `ldrb w8, [x8]; tbz w8, #0, #0x3be01c; add x0, sp, #0x30` | feito (wrapper) |
| `0x3c11fc` | `_ZN8CGarages6UpdateEv` | `ldr w8, [x8]; and w8, w8, #0xf; cmp w8, #0xc; b.ne #0x3c12bc` | periódico → gate no tique + contador 30 Hz |
| `0x3c275c` | `_ZN7CGarage6UpdateEv` | `ldrb w8, [x8]; tst w8, #0x1f; b.ne #0x3c3da4` | periódico → gate no tique + contador 30 Hz |
| `0x3c2d04` | `_ZN7CGarage6UpdateEv` | `ldr w8, [x8]; and w8, w8, #0x1f; cmp w8, #0x17; b.ne #0x3c3da4; sub x8, x29, #0x80` | periódico → gate no tique + contador 30 Hz |
| `0x3db0a0` | `_ZN11CRoadBlocks18GenerateRoadBlocksEv` | `ldr x23, [sp, #8]; ldr w8, [x8]; and w8, w8, #0xf` | periódico → gate no tique + contador 30 Hz |
| `0x416610` | `_ZN7CCopPed14ProcessControlEv` | `ldrh w9, [x19, #0x7a]; ldr w8, [x8]; add w8, w8, w9; and w8, w8, #0x1f; cmp w8, #0x11` | periódico → gate no tique + contador 30 Hz |
| `0x41bc3c` | `_ZN4CPed14ScanForThreatsEv` | `ldrh w9, [x19, #0x7a]; ldr w8, [x8]; add w8, w8, w9; tbz w8, #2, #0x41be78; ldrh w8, [x19,` | periódico → gate no tique + contador 30 Hz |
| `0x41f368` | `_ZN4CPed14ProcessControlEv` | `ldrh w9, [x19, #0x7a]; ldr w8, [x24]; add w8, w8, w9; tst w8, #0x1f; b.ne #0x41f388` | periódico → gate no tique + contador 30 Hz |
| `0x425688` | `_ZN4CPed4FleeEv` | `ldrh w10, [x19, #0x7a]; ldr w9, [x9]; add w9, w9, w10; tst w9, #7; b.eq #0x425894` | periódico → gate no tique + contador 30 Hz |
| `0x4269f4` | `_ZN4CPed13PlayFootStepsEv` | `ldrb w8, [x8]; tbnz w8, #0, #0x426b28` | periódico → gate no tique + contador 30 Hz |
| `0x4282dc` | `_ZN4CPed8SetGetUpEv` | `ldrh w9, [x19, #0x7a]; ldr w8, [x8]; add w8, w8, w9; add w8, w8, #5; tst w8, #7` | periódico → gate no tique + contador 30 Hz |
| `0x42d51c` | `_ZN4CPed9PreRenderEv` | `ldrb w8, [x8]; tbz w8, #2, #0x42d73c; ldr x0, [x19, #0x70]; ldr q0, [x8, #0xbc0]` | visual → só contador 30 Hz |
| `0x42ee28` | `_ZN4CPed4SeekEv` | `ldrh w9, [x19, #0x7a]; ldr w8, [x8]; add w8, w8, w9; add w8, w8, #0x1c; tst w8, #0x1f` | periódico → gate no tique + contador 30 Hz |
| `0x4424e4` | `_ZN4CPed10SetCarJackEP8CVehicle` | `ldrh w9, [x20, #0x7a]; ldr w8, [x8]; and w9, w9, #7; and w8, w8, #7; cmp w8, w9` | periódico → gate no tique + contador 30 Hz |
| `0x45b4dc` | `_ZN10CPlayerPed14ProcessControlEv` | `ldrh w9, [x19, #0x7a]; ldr w8, [x8]; add w8, w8, w9; tst w8, #0xf; b.ne #0x45b554` | periódico → gate no tique + contador 30 Hz |
| `0x463518` | `_ZN11CPopulation27RemovePedsIfThePoolGetsFullEv` | `ldr w8, [x8]; and w8, w8, #7; cmp w8, #4; b.ne #0x4635f8` | periódico → gate no tique + contador 30 Hz |
| `0x47f5b0` | `_ZN4CHud4DrawEv` | `ldrb w9, [x9]; tbz w9, #3, #0x47f63c; ldr x9, [x9, #0x118]` | feito (wrapper) |
| `0x47f608` | `_ZN4CHud4DrawEv` | `ldrb w9, [x9]; tbz w9, #3, #0x47f624; ldrb w9, [x25]` | feito (wrapper) |
| `0x47f630` | `_ZN4CHud4DrawEv` | `ldrb w8, [x8]; tbz w8, #3, #0x47f784` | feito (wrapper) |
| `0x47f740` | `_ZN4CHud4DrawEv` | `ldrb w9, [x9]; tbz w9, #2, #0x47f784` | feito (wrapper) |
| `0x4801c0` | `_ZN4CHud4DrawEv` | `ldrb w8, [x8]; tbnz w8, #2, #0x480124; cmp w22, w23; b.le #0x480168` | feito (wrapper) |
| `0x4801d8` | `_ZN4CHud4DrawEv` | `ldrb w8, [x8]; tbz w8, #2, #0x480168` | feito (wrapper) |
| `0x480b68` | `_ZN4CHud4DrawEv` | `ldrb w8, [x8]; tbnz w8, #2, #0x480b88; ldrh w8, [x23]; add x8, x24, x26` | feito (wrapper) |
| `0x4810f8` | `_ZN4CHud4DrawEv` | `ldrb w8, [x8]; tbz w8, #3, #0x48133c` | feito (wrapper) |
| `0x491144` | `_ZN8CRubbish6UpdateEv` | `ldr w9, [x23]; tst w9, w8; b.ne #0x491274` | periódico → gate no tique + contador 30 Hz |
| `0x491908` | `_ZN8CRubbish6StirUpEP8CVehicle` | `ldrh w9, [x0, #0x7a]; ldr w8, [x8]; eor w8, w8, w9; tst w8, #3; b.ne #0x491c00` | periódico → gate no tique + contador 30 Hz |
| `0x4b42d0` | `_ZN12CWaterCannon19Update_OncePerFrameEs` | `ldr w8, [x8]; add w8, w8, w20; tst w8, #3; b.eq #0x4b42f0; ldrb w8, [x19, #0x210]` | periódico → gate no tique + contador 30 Hz |
| `0x4b60bc` | `_ZN8CWeather6UpdateEv` | `ldrb w8, [x8]; tst w8, #7; b.ne #0x4b60e8` | periódico → gate no tique + contador 30 Hz |
| `0x4b7c40` | `_ZN10CCullZones6UpdateEv` | `ldr w8, [x9]; ldr x20, [x20, #0x190]; and w8, w8, #7; cmp w8, #6` | periódico → gate no tique + contador 30 Hz |
| `0x4c87d0` | `_ZN11CAutomobile15ProcessBuoyancyEv` | `ldrb w8, [x8]; tst w8, #0xf; b.ne #0x4c85b4; ldr q1, [sp, #0x20]` | periódico → gate no tique + contador 30 Hz |
| `0x4c948c` | `_ZN11CAutomobile14ProcessControlEv` | `ldrh w9, [x19, #0x7a]; ldr w8, [x8]; add w8, w8, w9; tst w8, #0xf; b.ne #0x4c94cc` | periódico → gate no tique + contador 30 Hz |
| `0x4cc1d4` | `_ZN11CAutomobile14ProcessControlEv` | `ldr w8, [x8]; and w8, w8, #7; cmp w8, #5; b.ne #0x4cc20c` | periódico → gate no tique + contador 30 Hz |
| `0x4cf1f4` | `_ZN11CAutomobile26AddDamagedVehicleParticlesEv` | `ldrh w9, [x19, #0x7a]; ldr w8, [x8]; add w8, w8, w9; tbnz w8, #0, #0x4cf7b8` | periódico → gate no tique + contador 30 Hz |
| `0x4cf528` | `_ZN11CAutomobile26AddDamagedVehicleParticlesEv` | `ldrh w9, [x19, #0x7a]; ldr w8, [x8]; add w8, w8, w9; tst w8, #7; b.ne #0x4cf7b8` | periódico → gate no tique + contador 30 Hz |
| `0x4cf658` | `_ZN11CAutomobile26AddDamagedVehicleParticlesEv` | `ldr w8, [x8]; ldrh w9, [x19, #0x7a]; add w8, w8, w9; tst w8, #0xf; b.ne #0x4cf7b8` | periódico → gate no tique + contador 30 Hz |
| `0x4cfacc` | `_ZN11CAutomobile20AddWheelDirtAndWaterER9CColPointj` | `ldrb w8, [x8]; tbnz w8, #1, #0x4cff08; ldr x8, [x8, #0x278]; ldr s8, [x8]` | periódico → gate no tique + contador 30 Hz |
| `0x4df014` | `_ZN5CBike26AddDamagedVehicleParticlesEv` | `ldrh w9, [x19, #0x7a]; ldr w8, [x8]; add w8, w8, w9; tbnz w8, #0, #0x4df2a8` | periódico → gate no tique + contador 30 Hz |
| `0x4df194` | `_ZN5CBike26AddDamagedVehicleParticlesEv` | `ldrh w9, [x19, #0x7a]; ldr q6, [sp]; ldr w8, [x8]; add w8, w8, w9; orr w8, w8, #2` | periódico → gate no tique + contador 30 Hz |
| `0x4df884` | `_ZN5CBike20AddWheelDirtAndWaterER9CColPointj` | `ldrb w8, [x8]; tbnz w8, #1, #0x4dfc88; ldr d0, [x0, #0x90]; ldr s2, [x0, #0x98]` | periódico → gate no tique + contador 30 Hz |
| `0x4e4b1c` | `_ZN5CBoat14ProcessControlEv` | `ldrh w9, [x19, #0x7a]; ldr w8, [x8]; add w8, w8, w9; tbz w8, #0, #0x4e4b58` | periódico → gate no tique + contador 30 Hz |
| `0x4e5284` | `_ZN5CBoat14ProcessControlEv` | `ldrh w9, [x19, #0x7a]; ldr w8, [x8]; add w8, w8, w9; tbz w8, #0, #0x4e53f4; add x0, sp, #0` | periódico → gate no tique + contador 30 Hz |
| `0x4e59a8` | `_ZN5CBoat14ProcessControlEv` | `ldrb w8, [x8]; tbnz w8, #1, #0x4e605c; ldr x8, [x8, #0xfb0]; ldrsh x9, [x19, #0x7c]` | periódico → gate no tique + contador 30 Hz |
| `0x4eedd0` | `_ZN5CHeli14ProcessControlEv` | `ldrh w10, [x19, #0x7a]; ldrb w11, [x19, #0x48c]; ldr w9, [x9]; add w22, w9, w10` | periódico → gate no tique + contador 30 Hz |
| `0x4f28d8` | `_ZN6CPlane14ProcessControlEv` | `ldr w9, [x19, #0x3b8]; ldrh w10, [x19, #0x7c]; ldr w8, [x8]; cmp w10, #0xc8; sub w21, w8, ` | periódico → gate no tique + contador 30 Hz |
| `0x4f2f9c` | `_ZN6CPlane14ProcessControlEv` | `ldrh w9, [x19, #0x7a]; ldr w8, [x8]; add w8, w8, w9; tst w8, #7; b.eq #0x4f2fc0` | periódico → gate no tique + contador 30 Hz |
| `0x4f3a88` | `_ZN6CPlane19TestRocketCollisionEP7CVector` | `add w8, w8, #0x7bf; add x2, sp, #8` | periódico → gate no tique + contador 30 Hz |
| `0x4f518c` | `_ZN6CTrain14ProcessControlEv` | `ldrh w9, [x19, #0x3b4]; ldr w8, [x8]; add w8, w8, w9; tst w8, #0xf; b.ne #0x4f513c` | periódico → gate no tique + contador 30 Hz |
| `0x502bc4` | `_ZN6CFerry14ProcessControlEv` | `ldr w10, [x10]; add w10, w10, w9; tst w10, #0xf; b.ne #0x502afc; ldr x8, [x8]` | periódico → gate no tique + contador 30 Hz |
| `0x5068a8` | `_ZN10CExplosion6UpdateEv` | `ldrb w8, [x8]; tbnz w8, #0, #0x5068f0` | visual: alterna luz/corona → só contador 30 Hz |
| `0x506d18` | `_ZN10CExplosion6UpdateEv` | `ldrb w8, [x8]; tbnz w8, #0, #0x506e28` | visual: alterna luz/corona → só contador 30 Hz |
| `0x508e40` | `_ZN9CShotInfo6UpdateEv` | `ldr w8, [x8]; add w8, w8, w21; tst w8, #3; b.ne #0x508c28; ldr s0, [x19]` | periódico → gate no tique + contador 30 Hz |
| `0x50ac2c` | `_ZN7CWeapon14FireInstantHitEP7CEntityP7CVector` | `ldrb w8, [x8]; tst w8, #3; b.ne #0x50ac4c; add x1, sp, #0xa0; sub x2, x29, #0x90` | periódico → gate no tique + contador 30 Hz |
| `0x50db20` | `_ZN7CWeapon21FireInstantHitFromCarEP8CVehiclebb` | `ldrb w8, [x8]; tst w8, #3; b.eq #0x50ddcc` | periódico → gate no tique + contador 30 Hz |
