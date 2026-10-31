#include/

`SKSEMenuFramework.h` is intentionally **not** part of this repository.

## Why

The header is part of [SKSE Menu Framework 3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3),
which is licensed **GPL-3.0**. Vendoring it here would place this repository
under GPL-3.0, which is incompatible with the license this project is released
under (see `../LICENSE`). It is therefore fetched by the builder instead.

Note that the header is not needed at all unless you want the in-game tuning
menu. The plugin builds and runs without it; the menu simply is not registered.

## How to obtain it

Take the **full-size** header (about 520 KB) — either from the framework's
`resources/` directory or from the example repository. The 1.8 KB header in
`include/` of the framework repository contains only bare `__declspec(dllexport)`
declarations, is used by the framework itself to compile, and will not work for
a plugin.

Place it here as:

```
include/SKSEMenuFramework.h
```

Then configure and build as described in `../docs/BUILDING.md`.

## If you skip it

`CMakeLists.txt` detects the missing header and builds a version without the
menu rather than failing, printing a notice during configuration. All corrections
still work and are driven by the configuration file; only the in-game tuning
menu is unavailable.
