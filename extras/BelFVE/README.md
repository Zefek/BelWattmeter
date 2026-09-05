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
| GPIO1 (TX0) | výstup | ladicí konzole, 115200 Bd | `Serial` |
| GPIO3 (RX0) | — | konzole, firmware z ní nečte | `Serial` |
| 3V3 | napájení | pull-up 470k detektoru, VDD2 izolátoru ADuM1201 | — |
| GND | zem | společná pro detekci i GND2 izolátoru | — |

Žádné další piny firmware nepoužívá — není tu LED, tlačítko ani I2C.

GPIO34 a 35 jsou **vstupy bez vnitřních pull-upů**, takže se externí 470k nemá čím přebít — to je důvod, proč právě ty, a ne jiné piny ADC1. ADC2 se zapnutou WiFi nefunguje vůbec. Obojí je fyzická vlastnost čipu, ne nastavení, takže se to nedá omylem rozbít v kódu.

> **Pozor na modul s PSRAM.** GPIO16 a 17 jsou volné jen na ESP32-WROOM. Na WROVER si je bere PSRAM a wattmetr by se musel přesunout na jiný pár pinů (`BEL_RX_PIN` v konfiguraci). Profil v `sketch.yaml` cílí na `esp32:esp32:esp32`, tedy generický Dev Module.

Před připojením BELu je nutné přepojit **VDD2 izolátoru ADuM1201 z 5 V na 3,3 V** a **pull-up detektoru taky na 3,3 V**; piny ESP32 nejsou 5V tolerantní. Detaily v [docs/PLAN.md](docs/PLAN.md).

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
| `BEL_RX_BUFFER_SIZE` | 4096 | RX buffer `Serial2`, viz níž |

**`BEL_RX_BUFFER_SIZE` musí pokrýt nejdelší zásek smyčky.** Při 9600 Bd je 1024 bajtů jen **1,07 s** dat, kdežto `loop max` v diagnostice ukazuje 1–5 s (TLS handshake při OTA kontrole). Kratší buffer se v takovém záseku přeplní, uprostřed rámce se ztratí bajty a `belFrameErrors` roste. Výchozí 4096 bajtů je 4,3 s, tedy nad rámec i nejhoršího naměřeného záseku. Druhá polovina řešení je nešahat na OTA po minutě — interval je v `secret.h` a pro provoz patří na hodinu.

`DET_STATE_OFF` má v `detector.cpp` fallback na 40, takže **starší `config.h` bez téhle konstanty se přeloží a hysterezi dostane taky** — nasazení nemusí čekat na úpravu config repa.

## Fáze 1: běh bez wattmetru (`BEL_ENABLED 0`)

Wattmetr se připojuje až v druhé fázi, takže uzel zatím jede jen s detekcí těles. Výchozí konfigurace je proto `BEL_ENABLED 0` a chová se takhle:

- **`TOPIC_FVE` se publikuje každých 60 s bez ohledu na wattmetr.** Payload má pořád všech 37 bajtů a stejné offsety, ale `voltage`, `current`, `power` i `consumption` jsou nuly.
- **`heaterState`, `dutyA` a `dutyB` jsou skutečné** — detekce běží naplno, protože na wattmetru nezávisí.
- **`energyA`/`energyB` zůstávají 0.** Přírůstek `consumption` je nulový, takže není co rozdělovat; jakmile se wattmetr připojí, začnou růst od nuly.
- **Availability je `Online` od startu a nikdy se nepřeklopí na `Offline`.** Časovač ticha BELu je vypnutý, protože „BEL mlčí“ je v téhle fázi normální stav. Last will zůstává funkční, takže mrtvý uzel se pozná dál.

Parser i `Serial2` běží dál a `belFrameErrors` se počítá — na nezapojeném vstupu by měl zůstat na nule. Pokud by tam číslo lezlo nahoru, chytá pin rušení. Callback z knihovny je ale v téhle fázi umlčený, takže i kdyby náhodou proletěl platný rámec, na drát se nedostane.

Druhá fáze je pak **změna jediné konstanty na `BEL_ENABLED 1`** — obě větve se kompilují, ověřeno.

## Co uzel loguje na sériovou linku

Každé okno vypíše `DETECTOR diag:` s počtem vzorků, stavem a pro oba kanály `avg` (v dílcích i v milivoltech), `min`, `max`, `duty`, `ripple`, počet odpojení a `adc=min..max`. **Nic z toho kromě `duty` a `ripple` není v MQTT kontraktu** — je to tam kvůli ladění.

`adc` je **surová 12bitová hodnota přímo z `analogRead()`**, jednou za sekundu na kanál, mimo hlavní vzorkovací cestu (ta jede dál přes `analogReadMilliVolts`). Slouží k jedinému: rozhodnout, jestli převodník v horní části rozsahu saturuje. Když `adc` max dosáhne 4095, vypíše se za tím `SATURACE` a je jasno — hodnota `raw` v diagnostice pak v té oblasti neměří napětí, ale strop ADC. Přepočet na stupnici 0–1023 zahazuje dva bity, takže z `min`/`max` v dílcích se to poznat nedá; proto ten surový odečet.

Když je uzel offline, okno se zaloguje jako `offline` / `diag-offline` a zahodí. Energie se ale připisuje i tak, takže výpadek MQTT nezpůsobí ztrátu `energyA`/`energyB`.
