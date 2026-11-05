# Tredia Studio · تريديا ستوديو

Tredia Studio is the desktop recording and live streaming app of [Tredia](https://tredia.cloud).
It is a fork of [OBS Studio](https://github.com/obsproject/obs-studio) 33.0.1 with Tredia's look,
Arabic first, and built-in Tredia features:

- **Sign in with Tredia** and the stream settings fill themselves in.
- **Turbo**: send one stream to Tredia, and Tredia's servers relay it to YouTube, Twitch, Kick and TikTok.
- **From your device**: stream to several places straight from your PC (uses your upload once per destination).
- **After a stream**: open the stream report on Tredia (best moment, viewers per platform, chat).
- **After a recording**: upload it to your Tredia content library, or send it to your editor.

تريديا ستوديو هو برنامج التسجيل والبث من تريديا، مبني على OBS Studio ومفتوح المصدر مثله.

## Licence

Tredia Studio is free software under the **GNU General Public License v2.0 or later**, the same licence
as OBS Studio. See [COPYING](COPYING). OBS Studio is © Lain Bailey and the OBS Studio contributors
([AUTHORS](AUTHORS)). "OBS" and "OBS Studio" are trademarks of the OBS Project; Tredia Studio is not made
or endorsed by the OBS Project.

The original OBS Studio README is kept at [docs/OBS-STUDIO-README.rst](docs/OBS-STUDIO-README.rst).

## Building

Windows x64 builds run on GitHub Actions ([.github/workflows/tredia-windows.yaml](.github/workflows/tredia-windows.yaml)).
Pushing to the `tredia` branch builds a zip; pushing a tag `tredia-vX.Y.Z` publishes a release.

To build locally you need Visual Studio 2026 (C++ desktop workload), CMake 3.28+ and Git, then:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
```
