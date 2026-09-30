# Third-party notices

Séance's own code (`host/`, `src/`, `test/`, `scripts/`, `packaging/`) is MIT-licensed (see `LICENSE`).

**Séance is an independent project and is not affiliated with, endorsed by, or maintained by the Ghostty project.**
"Ghostty" is the name of another project; it is used here only to describe what Séance is built on.

The binary releases bundle the following. Each keeps its own license; the copies we hold verbatim are in `licenses/`.

| Component | Role | License |
|---|---|---|
| [Ghostty](https://github.com/ghostty-org/ghostty) (libghostty core, with the small patches in `patches/`) | terminal core, renderer, config, shell integration | MIT — `licenses/Ghostty-LICENSE.txt` |
| [JetBrains Mono](https://github.com/JetBrains/JetBrainsMono) | default font (embedded in the core) | SIL OFL 1.1 — `licenses/JetBrainsMono-OFL.txt`, `JetBrainsMono-AUTHORS.txt` |
| [Symbols Nerd Font](https://github.com/ryanoasis/nerd-fonts) | icon glyph fallback (embedded in the core) | MIT — `licenses/NerdFontsSymbols-LICENSE.txt` |
| [iTerm2-Color-Schemes](https://github.com/mbadolato/iTerm2-Color-Schemes) (via Ghostty's theme bundle) | the 600+ bundled color themes | MIT (see upstream) |

Ghostty's core statically includes further open-source libraries. To the best of our knowledge these are: FreeType, HarfBuzz,
Fontconfig, libxml2, zlib, libpng, Oniguruma, glslang, SPIRV-Cross, simdutf, Google Highway, wuffs, stb, Dear ImGui, libxev,
zigimg, z2d, vaxis, uucode and zf, under permissive licenses (MIT, BSD, zlib, Apache-2.0, FreeType License).
See each project's repository for its exact terms; Ghostty's `build.zig.zon` lists the pinned versions.
This list is provided in good faith and is not legal advice.

Runtime dependencies that are *not* bundled (installed from your distribution): GTK 3, Mesa (EGL and the llvmpipe software
renderer), fontconfig, glibc.
