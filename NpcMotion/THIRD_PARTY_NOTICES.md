# Third-party notices

FF7 Rebirth NPC Motion is MIT licensed (see `LICENSE`). It includes or redistributes the following
third-party components.

## ReShade add-on headers and descriptor tracking

`third_party/reshade/` — Copyright (C) 2014 Patrick Mours. BSD 3-Clause License, see
`third_party/reshade/LICENSE.md`. <https://github.com/crosire/reshade>

`src/descriptors.{hpp,cpp}` are adapted from ReShade's `examples/utils/descriptor_tracking` —
Copyright (C) 2021 Patrick Mours, BSD-3-Clause OR MIT.

ReShade itself is not redistributed; users install it separately.

## Dear ImGui header

`third_party/imgui/` — Copyright (c) 2014-2025 Omar Cornut. MIT License, see
`third_party/imgui/LICENSE.txt`. <https://github.com/ocornut/imgui>

## DirectX Shader Compiler

Release packages include `dxcompiler.dll` from the official
[DirectX Shader Compiler v1.9.2609 release](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2609),
which the add-on uses to read shader reflection data. It is distributed under the LLVM Release License
(University of Illinois/NCSA) and the MIT License, included in the package as
`FF7RebirthNpcMotion/licenses/dxcompiler-LICENSE-LLVM.txt` and `dxcompiler-LICENSE-MIT.txt`.
