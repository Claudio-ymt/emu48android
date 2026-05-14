# Web/Emscripten version by Claudio_ymt

This directory contains the web/Emscripten adaptation of Emu48 Android.

## License

This web version is based on Emu48 Android.

Emulator-derived files are licensed under the GNU General Public License v2.0.
See `../LICENSE-GPL.TXT`.

## ROM file

This repository does not include calculator ROM files.

To run the emulator, place your ROM file in the required location:

\web\assets

Expected example:

\web\assets\rom.50g

The exact filename expected by the emulator is:

rom.50g

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

This generates the web build files used by the browser version.

## Run locally

From the `web/` directory:

```bash
python -m http.server 8000
```

Then open:

```text
http://localhost:8000/emu48.html
```

## Notes

The generated `.js` and `.wasm` files are build outputs. The corresponding source files and build script are included in this repository.

