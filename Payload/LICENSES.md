# Licenses for the Mod package

| Files | License and provenance |
| --- | --- |
| Project-written loader, enhancement DLL, launcher scripts, build scripts, and their source in `ModSource/` | MIT. The full text is in the repository's root `LICENSE` and `Payload/LICENSE`. |
| `Magpie/config.badmojo-windowed.json` | Project launch profile supplied for Magpie. MIT. |
| User's installed English Redux game | Not included. Its rights remain with its owners. |
| Magpie | Not included in this repository. When run, the installer downloads the latest stable x64 ZIP directly from the official Magpie GitHub release if `Magpie.exe` is absent; an existing user-installed copy is kept. Magpie is GPLv3; its bundled dependencies can have other licenses. |

The packaged `Magpie/` directory contains only the Mod's launch profile. This
package does not contain Magpie binaries, Microsoft UI Xaml, Magpie effects, or
Magpie source. Magpie runs as a separate process. This document does not claim
ownership of Magpie, Microsoft UI Xaml, or Bad Mojo Redux.
