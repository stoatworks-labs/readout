# Attributions

Readout is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is PROVISIONAL: hand-written in the shape the `stoatworks-backend` sync
(`scripts/sync-attributions.py`) generates. Readout is in that script's
`names.json` but not in its component lists, so the sync cannot produce this file
yet — v0.1.0 shipped with the hand copy. Once the registration is finished the
sync overwrites this file; edit it there, not here.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl, pinned to b1afaf9.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives through the vcpkg manifest on Windows only. Not fetched on macOS.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something this plugin calls — listed because it is present in the checkout. The harness writes PNGs through the system zlib and not through this.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
