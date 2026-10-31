# Building

## Prerequisites

| Component | Notes |
|---|---|
| Visual Studio 2022 | Desktop C++ workload, MSVC v143 toolset |
| CMake | 3.21 or newer |
| [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) | must be **built and installed** first |
| spdlog | consumed by CommonLibSSE-NG |
| SKSE Menu Framework header | optional, see [../include/README.md](../include/README.md) |

## Step 1 — build and install CommonLibSSE-NG

One installation can cover both runtimes; enable whichever you need.

```bash
cmake -B build -S . \
  -DENABLE_SKYRIM_SE=ON -DENABLE_SKYRIM_AE=ON -DENABLE_SKYRIM_VR=OFF \
  -DBUILD_TESTS=OFF \
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded \
  -DCMAKE_PREFIX_PATH="<path-to-spdlog-install>" \
  -DRAPIDCSV_INCLUDE_DIRS="<path-to-rapidcsv>/src" \
  -DCMAKE_INSTALL_PREFIX="<path-to-clib-install>" \
  -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target install
```

Two dependency notes, both learned the hard way:

- `-DCMAKE_PREFIX_PATH` must contain a **spdlog** installation, or
  `find_dependency(spdlog)` fails at configure time.
- `-DRAPIDCSV_INCLUDE_DIRS` must be set explicitly, otherwise the generate step
  fails because that cache variable is undefined.
- After `install`, if `CommonLibSSE.cmake` is missing from the install tree, copy
  it from the source tree's `cmake/` directory into
  `<prefix>/lib/cmake/CommonLibSSE/`.

## Step 2 — build the plugin

```bash
cmake -B build -S . \
  -DCMAKE_PREFIX_PATH="<path-to-clib-install>;<path-to-spdlog-install>" \
  -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The output is `build/Release/FurnitureUserSizeFix.dll`.

## Step 3 — optional: include the in-game menu

Place the full-size `SKSEMenuFramework.h` (about 520 KB, **not** the 1.8 KB
declaration-only file) at `include/SKSEMenuFramework.h` and re-run the configure
step. Configuration prints whether the menu is being compiled in.

That header is GPL-3.0 and is deliberately not redistributed by this repository.
Building with it links GPL-3.0-licensed interface code into your own binary;
decide for yourself whether that is acceptable for how you intend to distribute
what you build.

## Step 4 — verify before distributing

Check that the DLL exports the SKSE entry points and links only system
libraries:

- exported: `SKSEPlugin_Load`, `SKSEPlugin_Query`, `SKSEPlugin_Version`
- imported: only system DLLs (`kernel32`, `user32`, `ole32`, `version`, `shell32`);
  an import of `MSVCP140`/`VCRUNTIME140` means the dynamic CRT crept in and the
  plugin will fail to load on machines without the redistributable
- `DllCharacteristics` should read `0x160`

## Packaging

The released packages are built by the author. To produce your own:

```
<root>/
├─ fomod/{info.xml, ModuleConfig.xml}
├─ core/            FurnitureUserSizeFix.ini          → SKSE\Plugins
├─ dll-dual/        FurnitureUserSizeFix.dll          → SKSE\Plugins
├─ dll-se-only/     FurnitureUserSizeFix.dll          → SKSE\Plugins
├─ lang-chs/        FurnitureUserSizeFix_Lang.ini     → SKSE\Plugins
└─ README.txt
```

Zip the **contents** of that directory (so `fomod/` sits at the archive root),
otherwise the mod manager will not recognise it as an installer package.
