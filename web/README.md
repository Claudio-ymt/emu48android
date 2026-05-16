# Web/Emscripten version by Claudio_ymt

This directory contains the web/Emscripten adaptation of Emu48 Android, packaged as a mobile-first website at `index.html`.

## License

This web version is based on Emu48 Android.
Emulator-derived files (`emu48.js`, `emu48.wasm`, `emu48.data`) are licensed under the GNU General Public License v2.0.
See `../LICENSE-GPL.TXT`.

The custom site layout (`index.html` and its embedded CSS/JS) is the original work of Claudio_ymt and only loads the GPL emulator artifacts at runtime.

## Files

- `index.html` — mobile-first calculator page (loader, scroll buttons, ad slot, footer).
- `emu48.js` / `emu48.wasm` / `emu48.data` — Emscripten build outputs (GPL).
- `build.ps1` — build script for regenerating the Emscripten artifacts.
- `assets/` — location for ROM files (not included, see below).

## ROM file

This repository does not include calculator ROM files.
To run the emulator, place your ROM file in:

```
web\assets\
```

Expected example:

```
web\assets\rom.50g
```

The exact filename expected by the emulator is:

```
rom.50g
```

## Build requirements

You need Emscripten installed and activated.

Example:

```bash
cd path/to/emsdk
emsdk_env.bat
```

On PowerShell:

```powershell
cd path\to\emsdk
.\emsdk_env.ps1
```

## Build

From the `web/` directory, run:

```powershell
.\build.ps1
```

This regenerates `emu48.js`, `emu48.wasm`, and `emu48.data`.

## Run locally

From the `web/` directory:

```bash
python -m http.server 8000
```

Then open the custom site:

```text
http://localhost:8000/index.html
```

To test on a mobile device on the same Wi-Fi, find your machine's local IP (e.g. `ipconfig` on Windows) and open:

```text
http://:8000/
```

The original Emscripten test page is also available at:

```text
http://localhost:8000/emu48.html
```

## Notes

The generated `.js` and `.wasm` files are build outputs. The corresponding source files and build script are included in this repository.

## Contact

Made by Claudio_ymt — claudiojrrs@hotmail.com
