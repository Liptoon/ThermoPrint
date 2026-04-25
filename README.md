# thermoprint

A command-line thermal printer driver for Linux with an interactive TUI, supporting two printer families over Bluetooth.

## Supported Printers

**Fischero D11s** (AiYin/Fichero, Xiamen Print Future Technology)
- 96 px wide printhead, 203 DPI
- 14 mm wide labels, 30 mm or 50 mm long
- Transport: Classic Bluetooth SPP (RFCOMM), BLE fallback

**Cat Printers** — GT01, GB01, GB02, GB03, YT01, MX05, MX06, MX08, MX10, MXTP
- 384 px wide, 200 DPI
- 57 mm continuous roll
- Transport: BLE

## Dependencies

- [SimpleBLE](https://github.com/OpenBluetoothToolbox/SimpleBLE) — BLE scan and connect
- BlueZ development headers (`libbluetooth-dev`) — Classic BT SPP on Fischero
- [stb_image](https://github.com/nothings/stb) and [stb_image_resize2](https://github.com/nothings/stb) — image loading and resizing (bundled)

## Building

```sh
mkdir build && cd build
cmake ..
make
```

## Usage

```
thermoprint --fischero|--cat [OPTIONS]
```

Run without arguments to launch the interactive TUI, which scans for nearby printers and walks through connect, print, and settings menus.

### Device selection

```sh
--fischero              Fischero D11s label printer
--cat                   Cat roll printer (any supported model)
--device NAME|MAC       Connect to a specific device by name prefix or MAC address
--scan-timeout N        BLE scan timeout in seconds (default: 15)
```

### Actions

```sh
--info                  Print full device info
--status                Print status and battery level
--text TEXT             Print text supplied on the command line
--text-file FILE        Print text from a file
--print FILE            Print an image (PNG, JPG, BMP, ...)
--feed N                Feed N dots of paper
--form-feed             Advance to the next label gap (Fischero only)
```

### Text options

```sh
--font-size 1-5         Glyph size (default: 2 for Fischero, 3 for Cat)
--margin N              Pixel margin on all sides (default: 2)
--line-spacing N        Extra blank rows between lines (default: 1)
--no-word-wrap          Truncate long lines instead of wrapping
```

Font covers Latin and Polish characters (U+0020–U+017E).

### Print options

```sh
--dither ALGO           floyd-steinberg* | atkinson | mean-threshold | halftone | none
--copies N              Number of copies (default: 1)
--density 0-2           0=light  1=medium*  2=dark
```

### Fischero-specific options

```sh
--spp / --ble           Force transport (default: SPP preferred, BLE fallback)
--portrait              Print along the short label edge instead of the long edge
--label-size 30|50      Label long-edge length in mm (default: 30)
--paper-type 0-2        0=gap/label*  1=black-mark  2=continuous
--shutdown-time N       Auto-shutdown timeout in minutes
--factory-reset         Restore factory defaults
```

### Cat-specific options

```sh
--energy 0xNNNN         Thermal energy, 0x0000 (light) to 0xFFFF (dark, default)
```

### Misc

```sh
-v / --verbose          Show protocol hex dumps and BLE service details
-h / --help             Show full usage
```

## Examples

```sh
# Print a label with large text, 3 copies
thermoprint --fischero --text 'FRAGILE' --font-size 3 --copies 3

# Print a price label at high density
thermoprint --fischero --text 'Price: 9.99' --font-size 2 --density 2

# Print an image on a 50 mm label, landscape
thermoprint --fischero --label-size 50 --print label.png --dither atkinson

# Connect to a specific Fischero by MAC
thermoprint --fischero --device C8:48:8A:42:0F:AC --text 'hello'

# Print a receipt text file on a Cat roll printer
thermoprint --cat --text-file receipt.txt --font-size 2

# Print a photo on a Cat printer
thermoprint --cat --print photo.png
```

## Pairing the Fischero D11s

The Fischero uses Classic Bluetooth SPP. Pair it once before first use:

```sh
bluetoothctl
agent NoInputNoOutput
default-agent
pair XX:XX:XX:XX:XX:XX
trust XX:XX:XX:XX:XX:XX
quit
```

After pairing, `thermoprint` connects automatically by scanning for `FICHERO*` devices.

## Configuration

Settings changed in the TUI are saved to `~/.config/thermoprint/config.json` and restored on the next run. CLI flags override saved settings for that run but do not write back to the file.
