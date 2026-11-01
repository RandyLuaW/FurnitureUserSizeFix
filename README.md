<div align="center">

# Furniture User Size Fix

**An SKSE plugin that adapts body position and posture for player characters and NPCs of non-standard body scale when they use furniture — including an upper-body IK module for station-type furniture.**

[![License](https://img.shields.io/badge/license-source--available%20%7C%20GPL--3.0-lightgrey.svg)](LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Platform](https://img.shields.io/badge/platform-Skyrim%20SE%201.5.97%20%7C%20AE%201.6.629%20to%201.6.1170-lightgrey.svg)](#requirements)
[![Binaries](https://img.shields.io/badge/binaries-Nexus%20Mods-informational.svg)](#downloads)

English ｜ [简体中文](README.zh-CN.md) ｜ **Download: Nexus Mods**

> This repository hosts the **source code only**. Compiled packages are not
> published here — see [Downloads](#downloads).

</div>

---

## What this is

Skyrim authors furniture placement for a character of body scale 1.0. When a
character's effective scale differs from 1.0 the result is visible: the character
sinks into seats, sits too far forward or too far back relative to the surface,
and the hands fall below workbench surfaces so tools and ingredients clip through
the arms.

This plugin corrects that at runtime, on the actor that is currently using the
furniture:

- **Positional correction** — independent depth and height offsets, addressable
  by furniture model, by model plus seat marker, or by a single furniture
  reference.
- **Upper-body IK** — raises the hands to workbench level by rotating the upper
  arms instead of lifting the whole character, so the feet stay grounded.

It is a plugin-only mod: no ESP, no scripts, no record edits, and nothing is
written into the save file.

## How the effective scale is determined

```
effective scale = actor->GetScale() * node scale of "NPC" * node scale of "NPC Root [Root]"
```

A character can be made small in three different ways, and all three are covered:
the race's default height, the actor's own scale, and node scale baked into a
custom skeleton NIF. At scale 1.0 both correction factors evaluate to exactly
zero, so standard-proportioned characters are never touched.

## Requirements

| | |
|---|---|
| Game | Skyrim SE 1.5.97, or Skyrim AE 1.6.629 – 1.6.1170 |
| SKSE | 0.2.0.20+ (SE) / 2.2.3+ (AE) |
| Address Library | the file matching your runtime |
| Body mod | one that nulls the `DOBJ SAT1` entry (for example CBBE 3BA's `3BBB.esp`) |
| Optional | SKSE Menu Framework, for the in-game tuning menu |

## Downloads

Compiled packages are published on **Nexus Mods**, not in this repository. Two
files are provided there:

| File | Use |
|---|---|
| `FurnitureUserSizeFix-<version>-FOMOD.zip` | Main. Installer package; asks for your game runtime and menu language. |
| `FurnitureUserSizeFix-<version>-CHS.zip` | Optional. Simplified Chinese menu translation only. |

To build from source instead, see [docs/BUILDING.md](docs/BUILDING.md).

> **Runtime support.** Supported: Skyrim SE 1.5.97 and Skyrim AE 1.6.629 - 1.6.1170.
> **Skyrim 1.7.x (1.7.99 / 1.7.104 and later) is not supported yet** — those builds changed the Address Library database format and the plugin must be rebuilt against a newer CommonLibSSE-NG.

## Quick start

1. Install the downloaded package with your mod manager. The installer asks for
   your game runtime and your menu language.
2. Enable it. No plugin file is added, so load order does not change.
3. Sit at a chair or stand at a workbench. To tune a value, open the SKSE menu,
   adjust the sliders (they apply immediately), then press a Save button.

Configuration lives in `SKSE/Plugins/FurnitureUserSizeFix.ini` and is documented
inline. The interface is English by default; translations are separate optional
downloads.

## Building

See [docs/BUILDING.md](docs/BUILDING.md). In short: Visual Studio 2022, CMake,
CommonLibSSE-NG, and — only if you want the menu — the SKSE Menu Framework
header, which you obtain yourself (see
[include/README.md](include/README.md)).

## Compatibility

**Furniture Height Size Fix (FHSF)?** Not compatible. Both apply a body-scale
compensation while furniture is in use, so the correction happens twice and the
symptom inverts: the character floats above the seat or is pushed away from the
workbench. Enable only one of them. The furniture blacklist FHSF used is
unnecessary here — this mod provides finer-grained tiers.

**Heels Fix (Nexus mod 64442)?** Compatible, with one setting turned off. Heels
Fix is a Papyrus + NiOverride solution: it tracks the heel offset of the boots
and, while a character uses furniture, cancels the heel's root-node lift and
replaces it with pelvis and knee rotation. This mod writes its own scale
compensation to the same root node. The two are additive rather than destructive,
because NiOverride re-applies its offset every frame — so they can coexist, but
if Heels Fix's own scale correction stays enabled, characters get compensated
twice.

Keep Heels Fix's basic and advanced furniture fixes running, and **turn off its
scale correction option** (the one aimed at characters without heels). Let this
mod own the body-scale compensation. If the pose then looks slightly off, set
this mod's *residual body lift* to zero and keep only the arm angle.

The Heels Fix author notes that its scale correction necessarily conflicts with
furniture height fix mods; this mod is a direct replacement for exactly that
function, so the same limitation applies to it.

**FurnitureHandIK?** Not compatible. Its functionality is included here and
enabling both rotates the upper arms twice. Disable the separate plugin.

**Foot IK mods (Foot IK Fix, Mu Joint Fix)?** They work against the whole-body
lift used on station furniture: this mod raises the root node, a foot IK mod
pulls the feet back down. Prefer the IK page's *arm raise angle* and keep
*residual body lift* at zero. For Mu Joint Fix, follow the furniture option
described on its own page rather than mixing two height corrections.

**Improved Camera and first-person body mods?** Compatible — offsets are written
to both of the character's 3D graphs, so the correction stays correct when
switching between first and third person. Heels Fix's optional first-person
camera adjustment is best left off when Improved Camera is used, per its own
documentation.

**SexLab and other scene-driven furniture?** Excluded by default through the
`[Forbid]` section, because scene frameworks teleport actors onto furniture and
any positional correction would make them float.

**Two copies of this mod?** Do not enable two. Every offset would be applied
twice.

**RaceMenu's height slider?** Unverified whether that offset is visible to this
mod's scale read; see *Known limitations*.

## Known limitations

- The grindstone is treated as a straddled seat, not a standing station. Its leg
  clipping at small scales is a mismatch between leg spacing and the furniture's
  own width and is intentionally not addressed.
- Characters at scale 1.0 or above are not corrected by default. The menu offers
  an optional switch for them; that path has never been verified in game.
- All values are calibrated at body scale 0.65 and converted at runtime.

- **RaceMenu's height slider may not be picked up.** RaceMenu applies height as a
  node-level scale offset, while this mod reads the actor scale multiplied by the
  skeleton node scale. Whether that offset is visible to that read is untested —
  check `engineScale` and `effScale` on an entry line in the log.

## Safety

The plugin writes nothing to the save file: it stores no co-save data, registers
no scripts, adds no spells, perks or items, and edits no records. Every positional
correction is applied per frame while furniture is in use and reverted as soon as
the character stands up, so uninstalling leaves nothing behind.

## Disclaimer and licensing

### No warranty

This software is provided "as is", without warranty of any kind. Use it at your
own risk. Back up your save files as you would with any mod that changes actor
placement at runtime.

This project is not affiliated with, endorsed by, or sponsored by Bethesda
Softworks, ZeniMax Media, the SKSE team, or any mod hosting site.

### Free, source-available, and not for resale

This project is **source-available**, not OSI open-source software. It is free to
use, study, modify and redistribute — including in modified form — provided that
you credit the author, link back to this repository, state any changes, keep the
license and notice files, and **do not charge for it in any form**, including
packaging it into a paid product or a paid mod pack.

See [LICENSE](LICENSE) for the full terms and
[THIRD-PARTY-LICENSES.txt](THIRD-PARTY-LICENSES.txt) for bundled components.

### About the compiled plugin

The plugin links SKSE Menu Framework interface code for the optional in-game
menu. That header is GPL-3.0, so a binary built with it is a combined work that
must be distributed under GPL-3.0. To make that lawful while keeping the
source-available terms for everything else, this project is **dual licensed**:
the terms in [LICENSE](LICENSE) are the default, and **GPL-3.0** is offered as an
alternative (see section 11 of LICENSE and [LICENSE-GPL-3.0](LICENSE-GPL-3.0)).
Binaries that include the menu are distributed under the GPL-3.0 option; builds
without the menu header contain no GPL-3.0 code and stay under the default terms.

Compiled packages are distributed on Nexus Mods rather than from this repository,
which hosts source code only.
