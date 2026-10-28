# Exceptions to the GNU General Public License

Copyright (C) 2026 rollingrock and Fo4VR-Upscaler contributors.

Fo4VR-Upscaler is licensed **GPL-3.0-or-later** (see `LICENSE`) with the two
additional permissions below.

This Program is intended to be used with and to modify existing code
(the "Modded Code"). The purpose of this exception is to address issues
when an open source modding community interacts with potentially
proprietary code. In addition, the modding community often uses libraries
(the "Modding Libraries") under licenses that may be incompatible with
the GPL ("Modding Library Licenses").

## Modding Exception

As an exception, the authors give You the additional right to link the
code of this Program with the existing code that this Program is
intended to be used with or modify and to distribute linked combinations
including the two, subject to the limitations in this paragraph. Modded
Code permitted under this exception may link to the code of this Program
without causing the Modded Code and portion of the combined work
corresponding to the Modded Code to be covered by the GNU General Public
License. You must obey the GNU General Public License in all respects for
all of the Program code and other code used in conjunction with the
Program except the Modded Code covered by this exception. If you modify
this file, you may extend this exception to your version of the file, but
you are not obligated to do so. If you do not wish to provide this
exception without modification, you must delete this exception statement
from your version and license this file solely under the GPL without
exception.

## GPL-3.0 Linking Exception (with Corresponding Source)

Additional permission under GNU GPL version 3 section 7

If you modify this Program, or any covered work, by linking or combining
it with Modding Libraries (or a modified version thereof), containing
parts covered by the terms of Modding Library Licenses, the licensors of
this Program grant you additional permission to convey the resulting work.

Corresponding Source for a non-source form of such a combination shall
include the source code for the parts of Source-Available Modding
Libraries used as well as that of the covered work.

Corresponding Source for a non-source form of such a combination need not
include the source code for the parts of Proprietary Runtime Libraries
used, nor for any model, data or shader file distributed with them. That
source is not available to the licensors of this Program on any terms, and
a requirement to convey it would make this Program impossible to convey at
all.

A combination formed only on your own computer - for example when this
Program loads a Proprietary Runtime Library that you obtained yourself - is
not a conveyance, and nothing in this exception requires you to convey
anything on account of it.

## For the purposes of the exceptions above, the following terms have these meanings

**"Modded Code"** means Fallout 4 VR and the files distributed with it, including
`Fallout4VR.exe` and the Bethesda Game Studios Creation Engine code it contains,
its Direct3D 11 shaders and its data files, together with any other mod or plugin
loaded into the same process alongside this Program. A component named below as a
Proprietary Runtime Library is treated as such, and not as Modded Code, where this
Program links to or calls into it.

**"Modding Libraries"** means, together, the Source-Available Modding Libraries
and the Proprietary Runtime Libraries defined below.

**"Source-Available Modding Libraries"** means libraries whose complete
corresponding source is publicly available, including without limitation:

- the Fallout 4 Script Extender for Fallout 4 VR (F4SEVR) and the VR Address
  Library data it consumes;
- CommonLibF4 and its forks;
- the OpenVR / SteamVR API (`openvr_api.dll`), and any OpenXR loader, runtime or
  translation layer such as OpenComposite;
- the MIT-licensed FidelityFX DirectX 11 fork this Program may be built against.

Some of these are licensed on terms already compatible with the GNU General Public
License. They are named for completeness, and naming them grants nothing beyond
what their own licences already allow.

**"Proprietary Runtime Libraries"** means components supplied only in binary form,
whose corresponding source is not available, including without limitation:

- the NVIDIA NGX and DLSS runtime, including `nvngx_dlss.dll`, `nvngx_dlssg.dll`
  and the `nvsdk_ngx` static libraries;
- NVIDIA Streamline binaries, including `sl.interposer.dll`, `sl.common.dll`,
  `sl.dlss.dll` and any other `sl.*.dll` plugin;
- the AMD FidelityFX runtime binaries, including `amd_fidelityfx_*.dll`;
- the Intel XeSS runtimes, including `libxess.dll` and `libxess_dx11.dll`;
- ENB (`d3d11.dll`, `enbseries*`) and its published API headers;
- Direct3D, DXGI, and any graphics driver component supplied by Microsoft or a
  hardware vendor.
