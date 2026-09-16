<div align="center">

# 🔍 WXL Glue Zoom

**Scroll-wheel zoom for the character customization screen**

A WarcraftXL module that adds smooth, native-feeling zoom to the 3.3.5a character
creation screen — every character stays perfectly framed no matter how close you go.

![version](https://img.shields.io/badge/version-v1.0-blue)
![license](https://img.shields.io/badge/license-GPL--3.0-green)
![client](https://img.shields.io/badge/client-3.3.5a%20(12340)-orange)
![renderer](https://img.shields.io/badge/renderer-D3D9-red)



https://github.com/user-attachments/assets/314e35e0-8d90-45e8-a21b-2c9213ce860d


</div>

In the stock client the character creation screen is fixed at one distance — you can't
zoom in for a closer look at your character's face, gear, or details. **WXL Glue Zoom**
adds smooth mouse-wheel zoom with intelligent framing that keeps every race's head and
chest perfectly composed, no matter how close you go.

---

## ✨ Features

- **Scroll-wheel zoom** — smooth in and out with a uniform glide; every notch lands as a
  continuous motion, never a jump.
- **Framing that follows you** — as you zoom in, the character stays beautifully composed
  through the entire range. No drifting, no sudden jumps.
- **Every race, tuned** — all 20 race/gender pairs were visually calibrated at max zoom so
  the face and chest land at the same spot on screen for everyone.
- **Rotation** — click-drag rotates the character while zoomed in, exactly like the
  native rotation. Zoom stays where you set it.
- **Scoped** — only activates on the character customization screen; does nothing in the
  world, menus, or character select.
---

## 📦 Installation

1. Download the latest `wxl-glue-zoom.dll` from the [Releases](https://github.com/bozo-1/wxl-glue-zoom/releases) page.
2. Copy it into your client's `Extensions/wxl-glue-zoom/` folder.
3. Launch the client and open the character creation screen — scroll to zoom.
- **Known limitation** — The module currently only works with the custom interface and models attached that you can get from here https://drive.google.com/drive/u/3/folders/1CoWJfj5BoaNI8ZoeS2QdAEtcPfq3dbHK

The module is discovered and loaded by the WarcraftXL extension system; no configuration
needed.

---

## 🧰 Requirements

| Component | Requirement |
|---|---|
| 🎮 Game | World of Warcraft **3.3.5a** (build 12340) |
| 🧱 Client framework | **WarcraftXL** (wxl-core) |
| 🎨 Renderer | Direct3D **9** |
| 🔨 Build | wxl-core toolchain (MSVC, x86) for the DLL |

---

## 📁 Project structure

```
wxl-glue-zoom/
├── README.md                 ← this file
├── LICENSE
├── wxl.json                  ← hub manifest (id, version, listing)
├── src/
│   └── WxlGlueZoom.cpp       ← the whole module
└── store/
    └── description.md        ← hub store listing text
```

---

## 🙏 Credits

- **WarcraftXL** — client framework, RE infrastructure, and the offset database that made
  engine-native zoom and framing possible.
- **Blizzard Entertainment** — World of Warcraft, 3.3.5a.

---

## 📜 License

See [LICENSE](LICENSE). WarcraftXL and World of Warcraft assets remain property of their
respective owners; this project's code is licensed as stated in the LICENSE file.
