# ESP32-C5 Spectrum Analyser for CardputerZero

Plug an ESP32-C5 board running [`esp32c5-spectrum`](https://github.com/jamesjulier/esp32c5-spectrum) (or [ESP-SDR](https://github.com/ESPARGOS/esp-sdr)) into the CardputerZero's side USB port to view live 2.4 GHz and 5 GHz radio energy on the 320 × 170 screen.

## Features

- **Plug-and-Play USB Serial**: Automatically detects your ESP32-C5 board on `/dev/ttyACM*` or `/dev/ttyUSB*` (and `/dev/cu.usbmodem*` on macOS), sends `ID?` to identify the board, and streams live 1024-point FFT slices across 2.4 GHz and three 5 GHz bands.
- **Three Views**:
  - **Live + Waterfall**: Real-time power-averaged trace (red at the noise floor to green at +36 dB), optional Peak Hold and long Average lines, scrolling waterfall, and bottom channel busy scorecard naming the quietest channel.
  - **Density**: Persistence heatmap showing how often each dB level is reached at every frequency.
  - **Channels**: Bar chart of channel busyness (% of samples ≥ +6 dB above the noise floor) with the quietest channel highlighted.
- **Demo Mode (`D`)**: Built-in synthetic RF simulator when no USB board is attached.
- **Easter Eggs**: Type `microwave` to jump straight to 2.45 GHz (Channel 9), or enter the Konami code (`↑ ↑ ↓ ↓ ← → ← → B A`) for green-phosphor oscilloscope mode.

## Keyboard Controls

| Key | Action |
|---|---|
| `1` – `4` or `Tab` | Switch band (`2.4 GHz`, `5 GHz ch 36–64`, `5 GHz ch 100–144`, `5 GHz ch 149–165`) |
| `V` | Cycle view (`Live + Waterfall` → `Density` → `Channels`) |
| `←` / `→` | Move frequency cursor readout (`Shift` + `←`/`→` for fast step) |
| `Z` or `Enter` | Zoom in (40 MHz) around the cursor's Wi-Fi channel, or zoom back out |
| `O` | Toggle Chanalyzer-style Wi-Fi channel spectral masks |
| `P` | Toggle Peak Hold line |
| `A` | Toggle Long Average line |
| `Space` | Freeze / Resume trace |
| `C` | Clear history and Peak Hold |
| `D` | Toggle Demo Simulator mode |
| `H` or `/` | Show / hide keyboard controls overlay |
| `Esc` or `Q` | Clear cursor / unzoom / Quit |

## Credits & Licence

- **Radio firmware**: [ESP-SDR](https://github.com/ESPARGOS/esp-sdr) by Florian Euchner (ESPARGOS), licensed under **GPL-3.0**.
- **Spectrum analyser & UI**: [James Julier](https://github.com/jamesjulier/esp32c5-spectrum), licensed under **GPL-3.0-or-later**.
- Levels are shown in **dB above the noise floor**, never dBm (receiver gain is uncalibrated). Receive only.
