# Changelog

All notable changes to this project are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/); versioning is
`MAJOR.MINOR.PATCH` and applies to the feature set, not to the build target.

## [1.2.0] - 2026-11-01

### Added
- Optional switch to also correct characters at scale 1.0 or above. Disabled by
  default; when enabled the same linear relationship is extrapolated upwards,
  which for characters above 1.0 yields a negative factor and moves them
  downwards. This path has never been verified in game and the menu states so.
- Distributable installer package (FOMOD) offering the game runtime and the menu
  language as install-time choices.
- Simplified Chinese translation of the in-game menu, shipped as a separate
  optional download and selectable in the installer.

### Changed
- Plugin renamed to `FurnitureUserSizeFix`, including the DLL, configuration
  file, log file and menu section name.
- Version numbering unified: one feature version, with the build target expressed
  only in the package name suffix.
- Default residual body lift lowered from 5.30 to 0, so characters are no longer
  floated by default; lift is opt-in per model.
- Author field set to `RandyLua`.

### Fixed
- Configuration file rewrites no longer drop the arm-IK section: the writer now
  emits every section it reads.
- The menu no longer displays default values as if they were saved values; it
  now reports which tier the value came from, in three colour-coded states.
- Reloading the configuration from the menu no longer falls back to English when
  the translation file happens to be unreadable on that attempt.

## [1.1.0] - 2026-11-01

### Added
- Anniversary Edition support. A single build now targets both SE 1.5.97 and
  AE 1.6.629+, with the vtable index selected at runtime.
- Skeleton-node scale is included in the effective scale calculation, so
  characters made small through a custom skeleton NIF are corrected as well.

## [1.0.0] - 2026-11-01

First public-shaped release.

### Added
- Per-furniture depth and height offsets, addressable by model, model plus seat
  marker, single reference, station class and animation class, with a documented
  priority chain.
- Upper-body IK: rotates the upper arms to raise the hands while the root node
  stays in place, so the feet remain on the ground.
- In-game tuning menu with live sliders, tier-aware status display and save
  buttons.
- English interface with a file-driven translation mechanism: a language file
  whose presence switches the language, and absence returns to English. The
  game's language setting is deliberately not consulted.
- Probe mode for logging skeleton node heights while seated.
