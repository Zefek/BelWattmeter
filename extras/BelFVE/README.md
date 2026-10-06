# BelFVE

Samostatný ESP32 uzel pro FVE ohřev: čte wattmetr BEL přes knihovnu z tohoto repozitáře a k tomu detekuje, které topné těleso právě hřeje.

MQTT kontrakt je v [docs/MQTT.md](docs/MQTT.md), postup nasazení a odůvodnění návrhu v [docs/PLAN.md](docs/PLAN.md). Naměřené podklady drží repozitář **HeaterElementDetector** (testovací rig).

## Build

```
arduino-cli compile ./extras/BelFVE
arduino-cli compile ./extras/BelFVE --upload --port COM3
```

Profil `BelFVE_ESP32` v `sketch.yaml` bere knihovnu z `../../src`, takže se nemusí instalovat do `libraries`. `FW_VERSION` dodává build (`-DFW_VERSION=<run_number>`); bez něj se zkompiluje jako 0.

Před buildem musí existovat `config.h` a `secret.h`. Při nasazení je generuje `deployConfig`, lokálně:

```
cp extras/BelFVE/config_default.h extras/BelFVE/config.h
cp extras/BelFVE/secret_default.h extras/BelFVE/secret.h
```

Oba soubory jsou v `.gitignore`.

## Nasazení

Workflow `Deploy BelFVE ESP32 on Environment` (`.github/workflows/deployment_belfve.yml`) se spouští ručně. Bez `serial_port` postaví OTA balíček, se `serial_port` (`COM3`) nahraje přes kabel.

Než poprvé projde, musí existovat:

- **Environment `Home` v tomhle repozitáři** s proměnnými `REPO` (config repozitář) a `FIRMWARE_TARGET_PATH` (složka, kam se publikuje `firmware.bin` a `manifest.txt`). Jsou to proměnné prostředí **tohoto** repozitáře, takže si nesahají do zelí s LSSensorem ani Garage.
- **`BelFVE\deploy.ps1` v config repozitáři** — generuje `config.h` a `secret.h` do `extras/BelFVE`. Bez něj `deployConfig` skončí chybou.
- **OTA endpoint** odpovídající `OtaUrl` ze `secret.h`.

Přihlašovací údaje ke config repozitáři si bere runner z credential manageru, ne z GitHub secrets — proto workflow nepředává `secrets: inherit`, na rozdíl od starších verzí u LSSensoru a Garage, které ještě mířily na starší commit sdíleného workflow.

## Zapojení

| Pin | Směr | Funkce | Konstanta |
|---|---|---|---|
| GPIO34 (ADC1_CH6) | vstup | detekce, kanál A | `DET_PIN_A` |
| GPIO35 (ADC1_CH7) | vstup | detekce, kanál B | `DET_PIN_B` |
| GPIO16 (RX2) | vstup | data z BEL wattmetru, 9600 Bd 8N1 | `BEL_RX_PIN` |
| GPIO17 (TX2) | — | **nezapojeno**, komunikace je jednosměrná | `BEL_TX_PIN` = −1 |
| GPIO25 | vstup | otáčky ventilátoru A, FG signál | `FAN_TACH_PIN_A` |
| GPIO26 | vstup | otáčky ventilátoru B, FG signál | `FAN_TACH_PIN_B` |
| GPIO1 (TX0) | výstup | ladicí konzole, 115200 Bd | `Serial` |
| GPIO3 (RX0) | — | konzole, firmware z ní nečte | `Serial` |
| 3V3 | napájení | pull-up 470k detektoru, VDD2 izolátoru ADuM1201 | — |
| GND | zem | společná pro detekci, GND2 izolátoru i pro 12V zdroj ventilátorů | — |

Žádné další piny firmware nepoužívá — není tu LED, tlačítko ani I2C.

GPIO34 a 35 jsou **vstupy bez vnitřních pull-upů**, takže se externí 470k nemá čím přebít — to je důvod, proč právě ty, a ne jiné piny ADC1. ADC2 se zapnutou WiFi nefunguje vůbec. Obojí je fyzická vlastnost čipu, ne nastavení, takže se to nedá omylem rozbít v kódu.

GPIO32 a 33 zůstávají schválně volné. Jsou to poslední dva použitelné vstupy ADC1 a drží se pro případné **externí teplotní čidlo na desce detektoru** — viz konec [docs/MQTT.md](docs/MQTT.md). Ventilátory proto sedí na GPIO25/26, které ADC1 nejsou.

### Ventilátory (Sunon MF30101V1-1000U)

FG každého ventilátoru budí **optočlen**; obě domény jsou galvanicky oddělené a hrany počítá **hardwarový čítač PCNT**, ne přerušení.

```
strana ventilátoru (12 V, plovoucí)        strana ESP32 (3,3 V)
  +12 V ──── 4k7 ──── anoda                  kolektor ──── GPIO25 / GPIO26
  FG (3. vodič) ────── katoda                emitor ─────── GND ESP32
  GND ventilátoru ──── GND 12V zdroje        4k7 na 3,3 V, bez kondenzátoru

              ↑ PC814, bariéra 5 kVrms, obě země NESPOJENÉ ↑
```

Polarita vychází nezměněná: FG dole → LED svítí → fototranzistor sepne → pin dole. Ve firmwaru se kvůli optočlenu nemění nic.

**Proč proudová smyčka.** Dvanáctivoltový zdroj ventilátorů je napájený ze stejnosměrné sběrnice FVE, kterou seká IGBT můstek. Je sice izolovaný, takže jeho výstupní zem plave, ale přes kapacitu mezi vinutími se ta zem na spínacích hranách hýbe. Dokud k ní vedl zemní drát od ESP32, byl to **jediný galvanický most mezi doménami** a tekl jím posuvný proud. Optočlen ten most ruší a zároveň mění vstup z napěťového uzlu se 45 kΩ, kterému stačí desítky mikroampér, na proudovou smyčku, která potřebuje miliampéry.

**Dimenzování odporu.** `I_F = (12 − 1,1 − 0,6) / 4k7 = 2,2 mA`, tedy pod datasheetovým `If(max) = 5 mA` ventilátoru. PC814 je AC typ se dvěma antiparalelními LED, takže na polaritě vstupu nezáleží.

**Na straně ESP32 je 4k7 na 3,3 V a nic víc, hlavně žádný kondenzátor** — pomalá hrana na pinu byla jednou z příčin ztracených hran, viz níž. Silnější pull-up dává rychlou hranu, ale chce víc proudu z fototranzistoru: 0,71 mA. PC814 má `CTR` od 20 %, takže nejhorší kus by při 2,2 mA dodal jen 0,44 mA a nesaturoval. Osazené kusy saturují (naměřeno 1,48 V střední hodnoty na pinu, tedy nízká úroveň kolem 0,15 V); **po výměně optočlenu to ověřit znovu**. Kdyby nesaturoval, snížit odpor u LED na 2k2 (4,7 mA, pořád pod limitem), ne zeslabovat pull-up.

**Čítání dělá PCNT, ne přerušení.** Důvod viz [Proč PCNT a ne `attachInterrupt`](#proč-pcnt-a-ne-attachinterrupt) níž: obsluha přerušení na rozklepané hraně ztrácela 38 % hran. PCNT vzorkuje po 12,5 ns, má vlastní hardwarový filtr zákmitů (`FAN_PCNT_FILTER_NS`) a mezi hranou a číslem u něj není žádný software. Vzorkovací úloha detektoru by se sem nehodila — nejkratší půlperioda FG je kolem 1,1 ms, takže by na ni z 1ms ticku vycházel jeden vzorek.

> Oba ventilátory musí viset na **téže větvi za termostatem**. Na tom stojí `fanMismatchSlots` v diagnostice: rozdíl mezi nimi pak nemůže být termostatem, a je tedy poruchou. Zablokovaný rotor drží FG trvale na `V_H`, takže se na signálu nedá odlišit od vypnutého — tahle křížová kontrola je jediné, čím se mrtvý ventilátor pozná.

> **Pozor na modul s PSRAM.** GPIO16 a 17 jsou volné jen na ESP32-WROOM. Na WROVER si je bere PSRAM a wattmetr by se musel přesunout na jiný pár pinů (`BEL_RX_PIN` v konfiguraci). Profil v `sketch.yaml` cílí na `esp32:esp32:esp32`, tedy generický Dev Module.

Před připojením BELu je nutné přepojit **VDD2 izolátoru ADuM1201 z 5 V na 3,3 V** a **pull-up detektoru taky na 3,3 V**; piny ESP32 nejsou 5V tolerantní. Detaily v [docs/PLAN.md](docs/PLAN.md).

### Blokování izolátoru ADuM1201

Datasheet Rev. L, strana 28, `APPLICATIONS INFORMATION` → `PCB LAYOUT` — není to v žádné tabulce specifikací, jsou to tři věty ke konci dokumentu:

> Power supply bypassing is strongly recommended at the input and output supply pins.
>
> The capacitor value must be between 0.01 µF and 0.1 µF. The total lead length between both ends of the capacitor and the input power supply pin must not exceed 20 mm.

Text je odstupňovaný: **blokovat se „důrazně doporučuje", ale hodnota a délka vývodů jsou „must".** Kondenzátor tedy chybět může, ale když tam je, musí být 0,01–0,1 µF a do 20 mm od pinu.

**Osadit na obou stranách** — VDD1 proti GND1 a VDD2 proti GND2. Týká se to i nás: po přestěhování převodníku k wattmetru chodí napájení výstupní strany půl metru kabelu z ESP32, a takový přívod žádných 20 mm nesplňuje.

Dvě čísla ze stejného datasheetu, která patří k výkladu `belFrameErrors`:

- **Kapacita bariéry `CI-O` je 1,0 pF** (Table 8). Při strmosti 5 kV/µs na měřené sběrnici to dělá souhlasný proud kolem 5 mA — **galvanická izolace proti souhlasnému rušení nechrání.**
- **Dekodér má hlídací obvod:** když nedostane vnitřní pulz déle než ~5 µs, předpokládá nenapájenou vstupní stranu a **vynutí na výstupu výchozí stav.** Je to cesta, jak transient na bariéře vyrobí zákmit na VOA nezávisle na blokování napájení — a vysvětluje chyby, které po osazení kondenzátorů zůstanou. Odpovědí na ně je souhlasná tlumivka na kabelu, ne RC filtr na vstupu ESP32; při 104µs bitu na 9600 Bd tam pro filtr není prostor.

## Ladicí knoflíky

Všechny jsou v `config_default.h`, takže se dají změnit konfigurací bez zásahu do kódu:

| Konstanta | Výchozí | K čemu |
|---|---|---|
| `BEL_ENABLED` | 0 | zapíná zpracování wattmetru, viz níž |
| `DET_TH_STATE` | 100 | práh pro `duty` a `heaterState` |
| `DET_TH_CONDUCT` | 512 | práh vodivosti pro `ripple` a odpojení zátěže |
| `DET_STATE_MAJORITY` | 50 | kolik procent okna stav **rozsvítí** |
| `DET_STATE_OFF` | 40 | pod kolika procenty stav **zhasne** (hystereze) |
| `DET_DROPOUT_BATCHES` | 4 | kolik nevodivých dávek (×50 ms) je odpojení |
| `DET_DROPOUT_MIN_RUN` | 20 | kolik vodivých dávek (×50 ms) musí odpojení předcházet |
| `BEL_WH_PER_UNIT` | 10 | jednotka `consumption` z BELu ve watthodinách |
| `BEL_DELTA_MAX_UNITS` | 1000 | přírůstek, nad kterým se Δ zahodí jako nesmysl |
| `BEL_SILENCE_TIMEOUT_MS` | 150000 | jak dlouho ticho od BELu znamená `Offline` |
| `BEL_FAULT_MIN_VOLTAGE` | 150 | nad jakým posledním napětím je ticho podezřelé |
| `BEL_FAULT_MIN_POWER` | 300 | a nad jakým posledním výkonem zároveň |
| `BEL_FAULT_CLEAR_MIN_POWER` | 50 | jaký výkon musí BEL po návratu ukázat, aby se porucha smazala |
| `BEL_RX_BUFFER_SIZE` | 1024 | RX buffer `Serial2`, viz níž |
| `FAN_PULSES_PER_REV` | **4** | započtených **hran** na otáčku, viz níž |
| `FAN_PCNT_FILTER_NS` | 10000 | hardwarový filtr PCNT; kratší přechod se nezapočítá (strop ~12,7 µs) |
| `FAN_SLOT_MS` | 1000 | jak často se čítače odečítají |
| `FAN_SLOT_MAX_MS` | 5000 | delší slot se celý zahodí, jen se přebazuje čítač |
| `FAN_RUN_MIN_PULSES` | 10 | od kolika impulzů ve slotu se ventilátor počítá jako běžící |

**`BEL_RX_BUFFER_SIZE` má velkou rezervu, i když to tak nevypadá.** Wattmetr posílá **jeden blok za sekundu**, ne souvislý proud — rámec má asi 32 bajtů a při 9600 Bd trvá 33 ms, takže zbytek sekundy je linka tichá. Výchozích 1024 bajtů proto pokrývá zhruba **32 sekund** dat, do čehož se nejdelší naměřený zásek smyčky (1–5 s při TLS handshake) vejde s velkým přebytkem. Přetečení bufferu tedy není vysvětlením pro rostoucí `belFrameErrors`; hledej příčinu v napájení wattmetru za soumraku nebo v rušení na lince.

### `FAN_PULSES_PER_REV` je 4, a není to překlep

Ventilátor dává podle datasheetu **2 impulzy FG na otáčku** — `T = T1+T2+T3+T4`, tedy dvě celé periody. Ověřeno měřením 3. 10. 2026: perioda FG 2441 µs, což při 12 200 ot/min sedí na dvojku.

Jenže PCNT je nastavený tak, aby počítal **obě hrany** (`PCNT_CHANNEL_EDGE_ACTION_INCREASE` pro náběžnou i sestupnou). Konstanta proto neznamená „impulzů na otáčku", ale **„započtených událostí na otáčku"** — a to jsou **4**. Dvojka by dala dvojnásobek, tedy 24 400 ot/min, což je mimo katalog.

> Dřív, s přerušením, vycházelo na 4 i `FALLING` — spouštěl se na obou hranách (změřeno 3. 10. 2026 nástrojem [FanScope32](../FanScope32), 815,0 hran/s v režimu `CHANGE` i `FALLING`). **Nebyla to ale vlastnost jádra, jak tu dřív stálo.** Měřilo se s 33 nF na pinu, tedy na hraně s náběhem kolem 310 µs, a přesně tenhle symptom popisuje [arduino-esp32 #4172](https://github.com/espressif/arduino-esp32/issues/4172): hrana pomalejší než ~200 µs (náběh) nebo ~40 µs (sestup) spouští i na opačné hraně. Na rychlé hraně by `FALLING` nejspíš počítal jen sestupné — neověřeno.

## Proč PCNT a ne `attachInterrupt`

**Protože obsluha přerušení na tomhle signálu ztrácela 38 % hran.** Měřeno 4. 10. 2026 tak, že PCNT běžel chvíli vedle přerušení na témž pinu ve stejném okně:

| | hran/s | |
|---|---|---|
| PCNT bez filtru | ~1700 | každá pravá hrana **plus jeden zákmit** |
| PCNT s filtrem 10 µs | ~800 | pravé hrany; sedí na FanScope32 (815) na 2 % |
| přerušení | ~490 | **62 %** |

Hrana na pinu byla rozklepaná — na každou pravou hranu připadá zhruba jeden přechod kratší než 10 µs. Z toho se nedělala přerušení navíc, jak by člověk čekal, ale **chybějící**.

**PCNT čte výstup téhož vstupního budiče jako logika přerušení**, takže jeho 800 proti 490 dokazuje, že se hrana do číslicové podoby dostane celá a ztráta je výhradně v cestě přerušení.

Sešly se tu dvě známé chyby ESP32:

- **[Erratum GPIO-3.14](https://docs.espressif.com/projects/esp-chip-errata/en/latest/esp32/03-errata-description/esp32/gpio-edge-interrupts.html)**, všechny revize v0.0–v3.1. GPIO0–31 sdílejí jeden stavový registr přerušení. V taktu, kdy obsluha maže svůj bit, se nemůže aktualizovat celý registr, a **hrana navzorkovaná v tu chvíli se ztratí** — ztratí, ne zdvojí. Espressif proto nedoporučuje mít v jedné skupině vedle hranového přerušení žádné další; tady byla dvě, GPIO25 a 26. [Nezávisle změřeno](https://www.quantulum.co.uk/blog/esp32-edge-triggered-interrupt-bug/): hrany se ztrácely spolehlivě při odstupu kolem 2,4 µs.
- **[arduino-esp32 #4172](https://github.com/espressif/arduino-esp32/issues/4172)**. Pomalá hrana projde prahem vícekrát; doporučení je náběh pod 2 µs (10–90 %). Na pinu byl z dřívějšího filtru proti rušení `4k7 + 33 nF`, tedy `τ ≈ 141 µs` a náběh kolem 310 µs — stopadesátkrát víc.

Pomalá hrana udělala z každé pravé hrany dávku přechodů vzdálených jednotky mikrosekund, tedy přesně v měřítku, na kterém erratum polyká. Víc přechodů znamenalo víc přerušení, víc mazání sdíleného registru a víc příležitostí ztratit hranu, i hranu druhého ventilátoru.

Projevovalo se to jako údaj závislý na napájení desky: 487 na vlastním zdroji, 574 s jiným přívodem, 607 se zemí notebooku, 813 na USB. Výklad, který na to sedí, **ale ověřený není**: kolikrát pomalá hrana zakmitá, určuje šum na referenci ESP32, a ten nejvíc dodává spínaný HDR-15-5 — na USB v cestě nebyl. Ventilátoru ani signálu se žádná z těch změn nedotkla. **Hledat to na vstupu byla slepá ulička**, a stála dva dny.

Kondenzátor je od 4. 10. 2026 venku. Nevysvětlené zůstalo, proč histogram mezer ukazoval spíš na zhruba milisekundovou mrtvou dobu než na náhodné ztráty (žádné dvě hrany blíž než 800 µs, trojnásobné mezery jen 0,01 %).

> **Pro další projekty na ESP32:** nejvýš jedno hranové přerušení na skupinu pinů (GPIO0–31 / 32–39 / RTC), hrany pod 2 µs bez RC na pinu, a na čítání hran PCNT.

`DET_STATE_OFF`, `DET_DROPOUT_MIN_RUN` a všech pět `FAN_*` ladicích konstant (včetně `FAN_PCNT_FILTER_NS`) má v `detector.cpp` / `fan.cpp` fallback, takže **starší `config.h` bez nich se přeloží** — nasazení nemusí čekat na úpravu config repa.

**`FAN_TACH_PIN_A` a `FAN_TACH_PIN_B` fallback nemají**, stejně jako `DET_PIN_A`/`DET_PIN_B`. Číslo pinu je popis hardwaru, ne ladicí hodnota: tiše dosazený default by četl cizí pin a tvářil se, že ventilátory stojí. Chybějící konstanta má shodit překlad.

**Konstanty poruchy fallback záměrně nemají.** `TOPIC_FVE_FAULT` ani tři prahy nejsou v `BelFVE.ino` nijak podchycené, takže `config.h` bez nich **se nepřeloží**. Je to jiná třída konstanty než `DET_STATE_OFF`: u ladicí hodnoty je rozumný default lepší než rozbitý build, ale u názvu topicu ne — fallback by se tiše publikoval jinam a nikdo by si toho nevšiml, protože chybějící alarm nevypadá jinak než klidný provoz. Hlasitá chyba překladu je tady správná odpověď, zvlášť při migraci s dočasnými topicy `fve/data_2` / `fve/state_2`.

Důsledek: **`BelFVE\deploy.ps1` v config repozitáři musí generovat všechny čtyři konstanty poruchy a k nim `FAN_TACH_PIN_A`/`FAN_TACH_PIN_B` dřív, než se tenhle firmware nasadí.** Není to volitelný úklid, bez něj deploy neprojde.

## Fáze 1: běh bez wattmetru (`BEL_ENABLED 0`)

Wattmetr se připojuje až v druhé fázi, takže uzel zatím jede jen s detekcí těles. Výchozí konfigurace je proto `BEL_ENABLED 0` a chová se takhle:

- **`TOPIC_FVE` se publikuje každých 60 s bez ohledu na wattmetr.** Payload má pořád všech 38 bajtů a stejné offsety, ale `voltage`, `current`, `power` i `consumption` jsou nuly.
- **`heaterState`, `dutyA`, `dutyB` i `fanState` jsou skutečné** — detekce i otáčky běží naplno, protože na wattmetru nezávisí.
- **`energyA`/`energyB` zůstávají 0.** Přírůstek `consumption` je nulový, takže není co rozdělovat; jakmile se wattmetr připojí, začnou růst od nuly.
- **Availability je `Online` od startu a nikdy se nepřeklopí na `Offline`.** Časovač ticha BELu je vypnutý, protože „BEL mlčí“ je v téhle fázi normální stav. Last will zůstává funkční, takže mrtvý uzel se pozná dál.
- **`TOPIC_FVE_FAULT` je vykompilovaný pryč a topic nikdy nevznikne.** Detekce poruchy stojí na tichu BELu, které je tady normální stav — uzel, který poruchu rozpoznat neumí, nesmí publikovat ani `Ok`. V HA zůstane entita v `unknown`, což je poctivý stav.

Parser i `Serial2` běží dál a `belFrameErrors` se počítá — na nezapojeném vstupu by měl zůstat na nule. Pokud by tam číslo lezlo nahoru, chytá pin rušení. Callback z knihovny je ale v téhle fázi umlčený, takže i kdyby náhodou proletěl platný rámec, na drát se nedostane.

Druhá fáze je pak **změna jediné konstanty na `BEL_ENABLED 1`** — obě větve se kompilují, ověřeno.

## Co uzel loguje na sériovou linku

Každé okno vypíše `DETECTOR diag:` s počtem vzorků, stavem a pro oba kanály `avg` (v dílcích i v milivoltech), `min`, `max`, `duty`, `ripple`, počet odpojení a `adc=min..max`. **Nic z toho kromě `duty` a `ripple` není v MQTT kontraktu** — je to tam kvůli ladění.

`adc` je **surová 12bitová hodnota přímo z `analogRead()`**, jednou za sekundu na kanál, mimo hlavní vzorkovací cestu (ta jede dál přes `analogReadMilliVolts`). Slouží k jedinému: rozhodnout, jestli převodník v horní části rozsahu saturuje. Když `adc` max dosáhne 4095, vypíše se za tím `SATURACE` a je jasno — hodnota `raw` v diagnostice pak v té oblasti neměří napětí, ale strop ADC. Přepočet na stupnici 0–1023 zahazuje dva bity, takže z `min`/`max` v dílcích se to poznat nedá; proto ten surový odečet.

Za tím se každých 300 s vypíše `FAN diag:` s počtem slotů, `mismatch` a pro oba ventilátory otáčkami, procentem běhu, počtem nenulových slotů, počtem slotů **započtených do průměru** a surovým počtem impulzů.

Ta dvojice „nenulových / průměr z" je při ladění nejcennější. Rozdíl mezi nimi je počet **přechodových sekund**, které se do rychlosti nezapočítaly. Když je ten rozdíl velký proti celku, ventilátor cykluje často — a je to zároveň kontrola, že vynechávání přechodů dělá, co má. Surové impulzy jsou tam proto, že je to jediné číslo, které neprošlo žádným přepočtem přes `FAN_PULSES_PER_REV`.

Zahozený slot se hlásí zvlášť jako `FAN: slot <n> ms zahozen` — pár kusů při TLS handshake je normální, trvalý proud znamená, že se smyčka někde zdržuje.

Když je uzel offline, okno se zaloguje jako `offline` / `diag-offline` a zahodí. Energie se ale připisuje i tak, takže výpadek MQTT nezpůsobí ztrátu `energyA`/`energyB`.
