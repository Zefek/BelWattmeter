# BelWattmeter Arduino Library

Arduino library for reading **BEL single-phase energy meters** over a serial line. It decodes the meter's framed protocol into voltage, current, instantaneous power and cumulative consumption — useful for photovoltaic (FVE) and appliance energy monitoring.

## Features

- **Framed protocol parser:** Handles start/stop markers (`0xFC` / `0xFE`), byte unescaping (`0xFD`) and per-frame CRC validation.
- **Range checking:** Rejects implausible readings (voltage, current, power out of range) so glitches don't corrupt the output.
- **Running average:** Voltage, current and power are averaged across all frames received during the current window.
- **Timed callback:** Once per interval (default 60 s) the library invokes your callback with the averaged reading and resets the window automatically — no manual polling needed.
- **Injectable serial port:** The stream is passed to the constructor, so any `HardwareSerial` (Serial1/2/3) or `SoftwareSerial` works.

## Installation

Copy this folder into your Arduino `libraries` directory, or add it as a git submodule of your project.

## Usage

1. **Include the library, define a callback and construct with a serial port:**
   ```cpp
   #include <BelWattmeter.h>

   void onBelData(BelData data) {
     // data.voltage, data.current, data.power, data.consumption
   }

   BelWattmeter wattmeter(Serial1, onBelData);
   ```

   Pass a third argument to change the window length (milliseconds):
   ```cpp
   BelWattmeter wattmeter(Serial1, onBelData, 30000);  // every 30 s
   ```

2. **Open the port at 9600 baud in `setup()`:**
   ```cpp
   void setup() {
     Serial1.begin(9600);
   }
   ```

3. **Pump the parser in `loop()`:**
   ```cpp
   void loop() {
     wattmeter.Loop();
   }
   ```

The callback fires once per interval with the averaged reading, then the window resets automatically. It is skipped for a window in which no valid frame arrived.

Any `Stream` works — a `SoftwareSerial` instance can be passed instead of a hardware port on boards without a spare UART. See [`examples/BasicRead`](examples/BasicRead/BasicRead.ino) for a complete sketch; it uses `Serial1` on boards that have it (e.g. Mega) and falls back to `SoftwareSerial` otherwise (e.g. Uno), selected with `#if defined(HAVE_HWSERIAL1)`.

## API

| Member | Description |
|--------|-------------|
| `BelWattmeter(Stream& serial, BelDataCallback callback, unsigned long interval = 60000)` | Construct with the serial port the meter is wired to, the callback invoked once per window, and the window length in milliseconds. |
| `void Loop()` | Read all available bytes, decode complete frames and fire the callback when the window elapses. Call every loop iteration. |

The averaged reading is delivered only through the callback; the accumulator is reset internally after each callback.

`BelData` fields: `voltage`, `current`, `power`, `consumption`. `BelDataCallback` is `void (*)(BelData)`.

## Notes

- The meter transmits at **9600 baud**; a valid frame has a data length of 28 bytes.
- Averaging is integer-based and spans one window; `consumption` always reflects the latest frame (not averaged).
- The callback runs synchronously inside `Loop()`, in the context of the main loop. Keep it short — heavy work blocks serial reading and delays the next window. Do **not** call `Loop()` (or anything that re-enters it) from within the callback: it would nest into the main loop and grow the stack on every window, eventually overflowing it on a small MCU. Set a flag and act on it back in `loop()` instead.

## License

MIT — see [LICENSE](LICENSE).
