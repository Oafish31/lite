# LiteTex
Tool texturing ringan (C++17, OpenGL 3.3, SDL2, Dear ImGui). Cocok untuk i3-4170 + iGPU HD 4400.

## Fitur
- **Node texture (gaya Blender)**: Noise, Voronoi, Checker, Color, Color Ramp, Mix, Invert, Material Output (Albedo/Height/Roughness). Tileable, real-time.
- **Height -> Normal map** otomatis dari node.
- **Paint 3D langsung di model** (gaya Substance Painter): brush, hardness, opacity, eraser.
- **Bake High-poly -> Low-poly**: Normal map (tangent space) + Ambient Occlusion, dengan BVH dan multi-thread.
- **Normal Decal** (gaya Substance Painter): stamp rivet, screw, hex bolt, scratch, panel line, dent, bump langsung di model. Bisa load PNG sendiri (normal map atau height/grayscale, alpha = mask). Decal diproyeksikan 3D (bukan di ruang UV) sehingga tembus seam UV dengan mulus dan tidak melar di sphere. Ada rotasi (Q/E), kekuatan, inverse, rotasi acak, Undo (Ctrl+Z). Hasil decal ikut masuk ke normal.png saat export.
- Import OBJ, export PNG (albedo, normal, roughness, height, ao).

## Build via GitHub
1. Upload folder ini ke repo GitHub.
2. Tab **Actions** -> workflow **Build** jalan otomatis.
3. Download artifact `LiteTex-windows-x64` -> `LiteTex.exe` (statis, tanpa install apa-apa).

## Cara pakai
- RMB orbit, MMB geser, scroll zoom, LMB paint.
- Node editor: klik kanan = tambah node, drag pin untuk sambung, Delete = hapus.
- Bake: load low-poly (harus ada UV) -> load high-poly -> Bake Normal Map -> Export.
- Saat drag slider, preview 256px lalu otomatis full-res saat dilepas (biar ringan).

## Import (simple)
- **Tab Import**: tombol *Buka model OBJ...*, *Buka Normal Map...*, *Buka Albedo...* (dialog file Windows).
- **Drag & drop** file OBJ / PNG / JPG / TGA / BMP ke jendela -> muncul pilihan "pakai sebagai apa".
- Normal map dari DirectX (Unity/Unreal)? Centang **Normal map DirectX (flip Y)**.
- UI berupa tab per langkah: Import, Texture, Paint, Decal, Bake, Export. Alat (paint/decal) otomatis mengikuti tab aktif.
