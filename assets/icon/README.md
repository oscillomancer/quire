# Application icon

A quire stood on its spine with its leaves fanned open, which is also an
analog level meter: the leaves are the meter's scale, the outer right leaf its
red zone, and a clay needle swings from the sewn fold, its pivot.

| File | Use |
|---|---|
| `app.svg` | Master on Apple's icon grid (824 of 1024); source of `app.icns` |
| `app-square.svg` | Nearly full-bleed variant for Windows and Linux (`png/`, `app.ico`) |
| `app-small.svg` | Five broader leaves for 32 px and below, every platform |
| `png/app-<n>.png` | 16 to 1024 px, for Linux icon themes and the window icon |
| `app.ico` | Windows: 16, 24, 32, 48, 64, 256 (PNG entries) |
| `app.icns` | macOS: 16 to 1024, including the @2x sizes |

Colours come from the stock plugins' theme
(`plugins/suite/glue/include/supple/suite/script_theme.h`: walnut, parchment
tones, clay) and the GUI theme (`daw/gui/ui/src/theme.cpp`: the master-strip
red used for the red zone).

Everything here is generated. Change the geometry in `tools/icon/build.py`,
then rebuild and commit the results:

```sh
python3 tools/icon/build.py      # needs resvg or rsvg-convert
```
