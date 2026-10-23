# emusync header

`include/emusync.h` is the C API of [emusync](https://github.com/ilyalissoboi/emusync) 0.2.1, as
published with its v0.2.1 release (SHA-256
`d63604f13b6e2b774752fcdb7a9b876a0d8cd3b419e1e9fba92d08498e325aae`, from the release's
`SHA256SUMS`). Save sync (`Core/SaveSync.cpp`) loads the library at run time, so building PPSSPP
needs only this header; the library itself comes from the same release:
`Windows/EmuSync.props` (Windows), `cmake/EmuSync.cmake` (Linux, macOS) and the emusync AAR
(Android, `android/build.gradle.kts`). Update all of them together.
