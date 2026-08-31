# Nakara GZDoom Fork

A custom engine fork based on **GZDoom 4.14.2**, developed for use with **Nakara**.

This repository contains the engine-side source changes used by the project. Game-specific artwork, localization data, Windows icons, and other proprietary game resources are not included. Engine-side shaders required by these features are included.

## Features

Major additions and changes include:

- **Focus Highlight** and configurable **VisThruWall** actor rendering
- Refractive **Cloak** rendering
- **Depth of Field**, matrix-based **Motion Blur**, Ambient Light, and aspect-correction post-processing
- Optional projected **sprite shadows** and additional renderer LOD controls
- **Hardware line-distance culling** (`nk_line_distance_cull`), adapted from the LZDoom approach, with conservative portal/polyobject safeguards
- Configurable **ParticleTrail / ribbon** effects for sprites and models
- Batched underwater ambient particles and particle-based **fish school simulation**
- Navigation-aware **A_SmartChase** and additional custom actor movement helpers
- Decoded **texture disk caching**, background preparation, texture warmup, and GPU upload pacing
- **Multiple music slots**, crossfading, intro/loop/outro sequences, and synchronized music stems
- Shared keyboard/mouse/gamepad **input glyph** support
- XInput connection tracking, controller rumble, and Steam Deck input detection
- Windows **IME/raw-input handling improvements**
- Smooth `DesiredFOV` interpolation and additional presentation controls
- Nakara-specific standalone-game configuration and INI migration support

For a more detailed overview, see [`docs/NAKARA_ENGINE_CHANGES.txt`](docs/NAKARA_ENGINE_CHANGES.txt).

---

# Welcome to GZDoom!

[![Continuous Integration](https://github.com/ZDoom/gzdoom/actions/workflows/continuous_integration.yml/badge.svg)](https://github.com/ZDoom/gzdoom/actions/workflows/continuous_integration.yml)

## GZDoom is a modder-friendly OpenGL and Vulkan source port based on the DOOM engine

Copyright (c) 1998-2023 ZDoom + GZDoom teams, and contributors

Doom Source (c) 1997 id Software, Raven Software, and contributors

Please see license files for individual contributor licenses

Special thanks to Coraline of the EDGE team for allowing us to use her [README.md](https://github.com/3dfxdev/EDGE/blob/master/README.md) as a template for this one.

### Licensed under the GPL v3
##### https://www.gnu.org/licenses/quick-guide-gplv3.en.html
---

## How to build GZDoom

To build GZDoom, please see the [wiki](https://zdoom.org/wiki/) and see the "Programmer's Corner" on the bottom-right corner of the page to build for your platform.

# Resources
- https://zdoom.org/ - Home Page
- https://forum.zdoom.org/ - Forum
- https://zdoom.org/wiki/ - Wiki
- https://dsc.gg/zdoom - Discord Server
- https://docs.google.com/spreadsheets/d/1pvwXEgytkor9SClCiDn4j5AH7FedyXS-ocCbsuQIXDU/edit?usp=sharing - Translation sheet (Google Docs)
