# Banana-Zero

**Congratulations on your new Banana-Zero.** You now own a neural rendering subwoofer for your eyeballs. It does not
move air. It moves pixels, gently, in the direction NVIDIA's DLSS Neural Rendering model thinks they should go.

In plain words: Banana-Zero runs NVIDIA's DLSS Neural Rendering (NR) model inside DirectX 12 games that already use
DLSS. Every time the game finishes its own DLSS Super Resolution (SR) or Ray Reconstruction (RR) pass, the NR model
gets one more go at that same frame, at display resolution, and the result goes back into the game's picture. Press
`End` in game for a menu with sliders; the picture follows a slider while you drag it.

**RTX 50 series only.** NVIDIA's model does not run on anything older. We checked. It sulks.

This is a hobby project and it would love some company. See [Help wanted](#help-wanted).

## What's in the box

The release zip ([Releases](https://github.com/swaggypatrol/Banana-Zero/releases)) contains:

| File | What it does |
|---|---|
| `dxgi.dll` | The main unit. Stands in for Windows' own `dxgi.dll`, passes everything through, and runs the NR pass and the menu |
| `banana.nvngx.dll` | The crossover. A tiny bridge the NR model is called through (more on why [below](#how-it-works)) |
| `README.md`, `LICENSE.txt`, `THIRD_PARTY_NOTICES.txt` | The paperwork. Please read the paperwork |

**Not in the box:** `nvngx_dlssnr.dll`, the NR model itself (about 165 MB; its file properties say "NVIDIA DLSSNR").
It is NVIDIA's and not ours to hand out, so you bring your own, the way you bring your own room to a speaker.

## Specifications

| | |
|---|---|
| GPU | GeForce RTX 50 series |
| API | DirectX 12 |
| Game must have | DLSS Super Resolution or Ray Reconstruction switched on |
| Model | `nvngx_dlssnr.dll`, supplied by you |
| Tested on | RTX 5090, driver 32.0.16.1714, Windows 11, model file 310.8 |
| Frequency response | One frame in, the same frame out |
| Crossover point | Right after DLSS, before tone mapping, the HUD and frame generation |
| Distortion | Each pixel moves at most 1 EV brighter or darker by default. Hard limit, not a suggestion |

### Tested games

| Game | Put the files in | DLSS the game uses |
|---|---|---|
| The Witcher 3 (Epic, DX12) | `<game folder>\bin\x64_dx12\`, next to `witcher3.exe` | Ray Reconstruction + Frame Generation, through Streamline |
| The Last of Us Part II Remastered | The game's root folder, next to `tlou-ii.exe` | Super Resolution + Frame Generation, through Streamline |

Other DX12 games with DLSS should work the same way in principle. "In principle" is doing some heavy lifting in that
sentence; please report back.

## Installation

Like any good sub, placement matters.

1. Close the game.
2. Find the folder that holds the game's main `.exe` (see the table above).
3. Already have a `dxgi.dll` in there (ReShade, OptiScaler and friends)? Rename it to something like `dxgi.dll.bak`.
   Two `dxgi.dll`s cannot share one folder, and Banana-Zero does not chain to another one.
4. Copy in `dxgi.dll` and `banana.nvngx.dll` from the same zip, plus your own `nvngx_dlssnr.dll`.
5. Start the game with DLSS on. A small **green square** in the bottom-right corner for a few seconds means NR is
   running. That is the power LED. Enjoy the power LED.

**Upgrading:** replace both DLLs together (they check that they come from the same build). Keep `dlssnr.ini`.
Coming from v0.1.1 or older? Style and the Model sliders never reached the model there, so whatever you set them to
did nothing. From v0.1.2 they work, and those old settings take effect. If the picture looks different after the
upgrade, right-click Style and the Model sliders to hand them back to the model.

**Uninstalling:** delete `dxgi.dll`, `banana.nvngx.dll`, `dlssnr.ini`, `dlssnr.log` and `dlssnr.prev.log` from the
game folder, and optionally `%LOCALAPPDATA%\Banana-Zero`. Banana-Zero never modifies a single file of the game.

### Break-in period

There is none. It sounds its best on day one. We just always wanted to write that.

## The in-game menu

Press **`End`**. The menu only opens while the game is actually running DLSS, so not in the main menu. Close it with
`End` again, `Esc`, or the Close menu button.

- While the menu is open the game gets no keyboard or mouse input, and gets it all back when the menu closes.
- **Sliders apply while you drag**: the picture follows the mouse, step by step. No Apply button. We trust you.
- **Right-click a slider** to put it back to its default. `Ctrl`+click to type a number.
- A **hollow slider** means "not set, the model uses its own default". Click or drag it to take over; right-click to
  hand it back.
- Everything is saved to `dlssnr.ini` next to `dxgi.dll`, a second after the last change and when the menu closes.
  Only the keys you changed are written; your own comments and lines in the file stay put. Edit the file by hand while
  the game runs and it is picked up too.
- The status line at the top reads like `3840x2160 RR HDR  frames N  failed N  model builds N`: resolution, which DLSS
  the game uses, HDR or not, frames processed, failures, and how many times the model was built.

### The top row

| Control | What it does |
|---|---|
| **NR on** | The master power switch. Off means the game runs exactly as if Banana-Zero were a plain pass-through |
| **A/B: show original** | Shows the untouched picture until you click again. Not saved. The best tool in the box |
| **Freeze frame** | Every frame reuses one captured frame, so every slider is judged on the same picture. Closing the menu unfreezes |

### Tuning

Three knobs. For most people, three knobs is plenty.

| Control | Range | What it does |
|---|---|---|
| **Strength** | 0 to 1.5, default 1 | How much of the model's change lands on screen. **0 is bit-for-bit what DLSS made.** 1 is the model's change as it is; 1.5 adds half again. Want more or less? This one |
| **Colour** | 0 to 1.5, default 1 | How much of the model's colour change comes along. 0 keeps the game's own colours and takes only brightness. In The Witcher 3 the model cools the whole picture whenever there is a lot of sky in view, so the colours drift as you look around; 0 stops that |
| **Style** | Standard, Natural, Cinematic | The model's three built-in styles. NVIDIA has not documented what they do, so audition them. Takes effect on the next frame |

**Advanced (rarely needed)** is folded away below, on purpose.

*Model.* The model's own knobs. NVIDIA has not documented any of them. What they feed and where comes from taking the
model's DLL apart; what they look like is our best guess from their names. Each one takes effect on the next frame,
so you see it while you drag. Style, Local structure, Local tone, Auto mask and Skin structure also restart the
model's memory of earlier frames at every step, so the picture looks a touch rougher while you drag them and for a
frame or two after you let go.

| Control | Range | Our best guess |
|---|---|---|
| **Model intensity** | 0 to 1 | Blends the model's result back towards its input, inside the model. Much the same job as Strength, which does it on the HDR frame and goes further, so leave this at 1 |
| **Local structure** | 0 to 1.5 | Fine texture and small light-and-shade detail |
| **Local tone** | 0 to 1.5 | Local contrast |
| **Auto mask** | Off, On | Lets the model pick out skin by itself, so that Skin structure can treat it apart from everything else. Greyed out while a sky slider (Depth page) is away from 1: handed our map of the picture, the model switches its own off |
| **Skin structure** | follows local structure, or 0 to 1.5 | Texture on skin. Only does anything with Auto mask on (greyed out otherwise). Turn it down for less texture on faces |

*HDR encode.* Both tested games hand DLSS linear HDR values with no upper limit, even with Windows in SDR mode, but
the model expects a finished-looking picture. So Banana-Zero shows it a stand-in: the frame divided by a **white
point**, encoded like a display would, with highlights above the white point folded into a smooth **shoulder**.

| Control | Range | What it does |
|---|---|---|
| **White point from** | Auto, Game exposure, Manual, Scene meter | What counts as "white". Auto (default) uses the game's exposure when the game provides it (TLOU2 does) and meters the scene when it does not (Witcher 3) |
| **White point (EV)** | -2 to +2, default 0 | Shifts the white point. +1 hands the model a picture one stop darker |
| **Highlight shoulder** | 0.50 to 0.90, default 0.70 | Where highlight compression starts, as a fraction of the white point |

*Composite.* How the model's answer goes back into the real frame.

| Control | Range | What it does |
|---|---|---|
| **Max gain (EV)** | 0 to 1.5, default 1 | The hard limit on how far any one pixel may move. Bright specks, noise or white fringes? Lower this first |
| **Highlight restore** | 0 to 1, default 0 | Scales the model's change inside compressed highlights back up. Made the Witcher 3 sky noisy once, hence 0 |

### Compare

| Control | What it does |
|---|---|
| **Split screen (original left, NR right)** + **Divider** | Side-by-side at the divider. Pair it with Freeze frame |
| **Preview window** | Off, Model input (what the model sees), Model output (what it returns). Comes with a histogram and a few readings |
| **Zebra stripes** | On the Model input preview: purple where highlights enter the shoulder, red where they are squeezed more than 3 EV. Purple belongs on skies, lamps and reflections, not on walls and faces. Healthy scenes showed roughly 3 to 16% purple |
| **Calibration card** | Draws a test chart onto a frozen frame, with grey steps, mid grey, white, a bright dot and colour patches, all in units of the white point. Put the split-screen divider through the middle: the two halves should look the same. Where they differ, that knob has gone too far |
| **Card corner** | Where the card goes |

### Depth (experimental)

Two experiments with something the game hands DLSS anyway: how far away every pixel is. Neither does anything until
you touch it, and Reset all puts both back.

| Control | Range | What it does |
|---|---|---|
| **Dilate by depth** | Off, On | Each motion vector takes the motion of whatever is nearest among itself and its four neighbours, so the edge of a moving thing moves with the thing, not with what is behind it. `nrprobe --tuning` measured it a little better over most of the frame and a little worse right on thin moving things. Judge it in motion: pan the camera past poles, branches and railings against the sky |
| **Sky local tone** | 0 to 1.5, default 1 | Local tone on the sky alone, as a multiple of Local tone. 1 is the same as the rest of the picture, 0.5 half as much, 0 none. Watch cloud contrast and smooth gradients |
| **Sky local structure** | 0 to 1.5, default 1 | The same for Local structure: the fine texture in clouds. Turn it down if the sky looks grainy |
| **Show what counts as sky** | Off, On | Purple stripes over what the sky sliders work on. Not saved |

The sky is whatever sits at the far end of the game's depth: the sky dome, and anything else the game never gave a
depth. Indoors, unless a window shows the sky, there is none and the sky sliders have nothing to work on; Show what
counts as sky tells you. The model looks at the picture as a whole, so what the sky gets moves the rest of the
picture too, by about a fifth as much.

### Speed (experimental)

The model costs about 7 ms a frame at 4K on an RTX 5090, nearly all of it inside the model, and its time follows the
number of pixels it looks at. This page trades some of its effect for some of that time. DLSS is not touched: the game
renders and upscales exactly as before, and only the model's own copy of the frame gets smaller.

| Control | Range | What it does |
|---|---|---|
| **Model input size** | 50% to 100%, default 100% | The model looks at a copy of the frame this big on each side. What it changes is carried back to the full frame along the full frame's own edges, so outlines stay sharp; what thins out is the finest of the model's own detail. 100% is the model on the whole frame, as before. Applies while you drag; each new size restarts the model's memory of earlier frames, so the picture looks a touch rougher for a frame or two |

Under the slider: the size the model actually gets, and **NR on the GPU**, the whole pass and the model's share of it in
milliseconds, measured on the GPU while the menu is open (the median of the last 16 frames).

`nrprobe` timed the model alone on an RTX 5090 at 4K: 7.1 ms at 100%, 4.4 at 75%, 3.8 at 67%, 2.9 at 50%. The slider
stops at 50% because about 1.7 ms of the model's time does not shrink with the picture: below that, the picture
would keep thinning for very little time back. Judge the trade with A/B and Split screen.

### Keys and Menu

| Control | Default | What it does |
|---|---|---|
| **Menu** | End | Opens and closes the menu |
| **A/B toggle** | not bound | A/B without opening the menu |
| **Freeze frame** | not bound | Freezes and opens the menu; press again to unfreeze |
| **Theme** | Kraft paper | Kraft paper or Dark |
| **Menu colour** | Auto | Auto, SDR, HDR. Pick by hand if the menu looks too dark, too bright or washed out on an HDR display |
| **Menu brightness (nits, HDR)** | 200 | How bright the menu's white is in HDR, 80 to 400 |
| **Green badge (seconds)** | 3 | How long the power LED shows. 0 turns it off, for the minimalists |

To bind a key, click its button and press the key. `Esc` cancels, Clear unbinds.

### Suggested tuning session

1. Stand somewhere with both bright and dark areas. Tick Freeze frame.
2. Click A/B or turn on Split screen and see what NR actually changes. Adjust Strength to taste, pull Colour down if
   colours drift, try each Style.
3. That is usually it. For extra credit, open Advanced, set Preview window to Model input and check the stand-in looks
   like your screen: too bright, push White point (EV) up; too dark, push it down.
4. Unfreeze and walk around. Look at lamps, bright windows and skies for halos, noise or fringes. Found some? Lower Max
   gain, then Strength.

## dlssnr.ini

Optional, lives next to `dxgi.dll`, one `Key = value` per line. The menu writes it for you; this is for people who
like to open the back panel.

| Key | Values | Default | In the menu |
|---|---|---|---|
| `Enabled` | 1 / 0 | 1 | NR on |
| `DetailStrength` | 0 to 2 | 1 | Strength |
| `ColourStrength` | 0 to 2 | 1 | Colour |
| `Style` | 0 standard, 1 natural, 2 cinematic | not set (standard) | Style |
| `Intensity` | 0 to 1; more acts as 1 | not set (1) | Model intensity |
| `LocalStructure` | 0 to 2 | not set (1) | Local structure |
| `LocalTone` | 0 to 2 | not set (1) | Local tone |
| `SkinStructure` | 0 to 2, -1 follows local structure; only with `AutoMask = 1` | not set (-1) | Skin structure |
| `AutoMask` | 1 / 0 | not set (0) | Auto mask |
| `Preset` | integer | not set | file only; model 310.8 carries a single set of weights, so every number gives the same model |
| `InputType` | `auto` / `linear` / `tonemapped` | `auto` | file only; `auto` follows the game's own HDR flag |
| `WhiteSource` | `auto` / `exposure` / `scene` / `manual` | `auto` | White point from |
| `WhiteEV` | -4 to +4 | 0 | White point (EV) |
| `Shoulder` | 0.5 to 0.95 | 0.70 | Highlight shoulder |
| `MaxGainEV` | 0 to 2 | 1 | Max gain (EV) |
| `HighlightRestore` | 0 to 1 | 0 | Highlight restore |
| `Compare` | 1 / 0 | 0 | Split screen |
| `CompareSplit` | 10 to 90 | 50 | Divider |
| `Preview` | `off` / `input` / `output` | `off` | Preview window |
| `Zebra` | 1 / 0 | 1 | Zebra stripes |
| `CardCorner` | `topleft` / `topright` / `bottomleft` / `bottomright` | `bottomleft` | Card corner |
| `MenuKey` | virtual-key code, hex | `0x23` (End) | Menu |
| `ToggleKey` | same, 0 = not bound | 0 | A/B toggle |
| `FreezeKey` | same | 0 | Freeze frame |
| `MenuTheme` | `paper` / `dark` | `paper` | Theme |
| `MenuColour` | `auto` / `sdr` / `hdr` | `auto` | Menu colour |
| `MenuNits` | 80 to 400 | 200 | Menu brightness |
| `Badge` | 0 to 10 seconds | 3 | Green badge |
| `DilateMotion` | 1 / 0 | 0 | Dilate by depth |
| `SkyTone` | 0 to 2 | 1 | Sky local tone |
| `SkyStructure` | 0 to 2 | 1 | Sky local structure |
| `ShowSky` | 1 / 0 | 0 | Show what counts as sky; the menu never saves it |
| `ModelScale` | 50 to 100 | 100 | Model input size |
| `StatsLog` | seconds, 0 = off | 0 | file only; a statistics line in the log every N seconds (`tools/bzstats.py` reads them) |
| `DumpEvery` | seconds, 0 = off | 0 | file only; a frame dump to `%LOCALAPPDATA%\Banana-Zero\dumps` every N seconds, at most 16 (`tools/bzdump.py` turns them into pictures) |

The file accepts wider ranges than the menu's sliders. Freeze, the calibration card, the sky's stripes and A/B are
never saved.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| No green square | DLSS is off in the game, the files are in the wrong folder, or `nvngx_dlssnr.dll` is missing. Read `dlssnr.log` next to `dxgi.dll`; it says what happened and why |
| `End` does nothing | The game is not running DLSS at that moment (main menu, loading screen), or another tool uses End. Rebind `MenuKey` |
| The game crashes at start | Another `dxgi.dll` or overlay is fighting for the same spot. Try with only Banana-Zero |
| Halos, noise, white fringes | Lower Max gain (EV), then Strength |
| Colours look off | Lower Colour |
| Menu too dark or washed out in HDR | Set Menu colour by hand and adjust Menu brightness |
| Something failed | Any failure means "no NR on this frame" and the game carries on as normal. `dlssnr.log` and `dlssnr.prev.log` (the previous session) have the details. Attach them to an issue |

## Known limitations

- **DirectX 12 only.** Vulkan is the next milestone. DX11 is not planned.
- **Tested in exactly two games.** The `dxgi.dll` export list comes from Windows 11; other Windows versions are untested.
- **It costs frame time.** About 7 ms per frame for the whole NR pass on an RTX 5090 at 4K output, nearly all of it
  inside the model. The Speed page shows what it costs on your machine, and its Model input size trades some of the
  effect for time.
- **The model's knobs are undocumented.** `nrprobe --tuning` measured that they act and what their defaults are; what
  each one does to the picture is our best guess from its name.
- Night scenes in Witcher 3, the pause menus and photo modes have not been tested specifically.
- The menu keeps keyboard and mouse from the game, but not a gamepad.

## Version history

| Version | What changed |
|---|---|
| v0.1.2 | The knobs are connected now. Style and the five Model knobs were handed to the model only when it was created, but the model reads them every frame, so in v0.1.1 they did nothing. They now act from the next frame, every slider applies while you drag, and turning a knob no longer rebuilds the model. New: `nrprobe --tuning` measures what the knobs do |
| v0.1.1 | The first public release. The menu and this manual learned English |

## How it works

- `dxgi.dll` forwards every export of the real `System32\dxgi.dll` (generated stubs, see `tools/gen_dxgi_exports.py`),
  so the game gets its DXGI as usual.
- When the game loads the driver's NGX runtime (`_nvngx.dll`), Banana-Zero patches five of its exports: Create,
  Evaluate, Release, Shutdown and Shutdown1. The game's own DLSS calls go through unchanged, byte for byte, return
  values included.
- After a successful SR or RR evaluate, on the same command list, the NR pass encodes the stand-in picture, runs the
  model 1:1 at display size (no upscaling, it does not replace DLSS), and composites the result back.
- The composite takes only the model's *ratio* of change per pixel and applies it to the untouched HDR frame, capped
  by Max gain. That is why the model cannot make the whole picture blow out or go dark.
- With Model input size below 100% the model gets a smaller copy instead, each of its pixels the average of the frame
  pixels it covers. Its change comes back to full size through a guided filter: fitted in small windows against the
  brightness of the model's input, applied to the brightness of each full-size pixel, and never more than the model
  itself changed at the four nearest of its pixels. The model is not rebuilt for this; each frame it is simply handed
  a smaller part of its input and output.
- The model only uses what the game already gives DLSS: colour, depth and motion vectors. No per-game patching.
- **Why the bridge DLL?** The model only accepts callers whose file name contains `nvngx.dll`, and the driver's NGX
  runtime refuses to load the model itself because of its signature. So `dxgi.dll` calls it through
  `banana.nvngx.dll`, which knows no parameter names and just passes calls along.
- Teardown order matters: the model shuts down first, then the NGX runtime.
- Banana-Zero does not touch ray tracing, path tracing, sharpening, frame generation or Reflex.

## Building

You need Visual Studio 2022 (Build Tools are enough, toolset v143), Windows SDK 10.0.26100.0 (its `dxc.exe` compiles
the shaders), and `git` and `python` on your `PATH`.

```
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" dlssnr.sln -p:Configuration=Release -p:Platform=x64 -v:minimal -m
```

The output lands in `build\Release\`: `dxgi.dll`, `banana.nvngx.dll`, and the test programs. Both DLLs carry a build
stamp from `git describe --always --dirty --abbrev=7` and refuse to work with a partner from a different build.

`dlssnr.log` lives next to `dxgi.dll`. Each line starts with seconds since the log opened; it records the build stamp,
which real `dxgi.dll` was loaded, when the NGX runtime was hooked, each Create and Release, the NR setup, menu events
and a periodic report.

### Tests

Run these from the repository root after a build. The exit code is the number of failures, so 0 means pass.

| Program | Needs | Checks |
|---|---|---|
| `build\Release\dxgitest.exe` | nothing special | The export table matches the real `dxgi.dll`, every export reaches its real function, and a DXGI factory and D3D12 device can be made through ours |
| `build\Release\ngxtest.exe` | nothing special | The five hooks against a fake NGX runtime and the bridge against a fake model |
| `build\Release\menutest.exe` | nothing special | Draws the panel with no GPU, drags sliders with fake mouse events, checks that every step of a drag applies at once, right-click reset and the ini write-back |
| `build\Release\shadertest.exe` | any D3D12 GPU | Runs the shaders on synthetic frames and compares them with the same maths on the CPU |
| `build\Release\nrprobe.exe [--model <path>]` | RTX 50 + `nvngx_dlssnr.dll` | Creates and evaluates the real model on the real GPU, then tears down in the same order as `dxgi.dll` |
| `build\Release\nrprobe.exe --tuning` | the same | Also measures what the model does with its knobs: that each one acts when written every frame, the model's own defaults and clamps, which motion-vector scale it wants, whether motion vectors dilated by depth help it along moving edges and thin bars, whether a control mask sets it per pixel, and, given Witcher 3 frame dumps (`DumpEvery`), how each setting moves the colours |

### Regenerating the export stubs

`src/dxgi.def`, `src/dxgi_stubs.asm` and `src/dxgi_exports.h` are generated from the real `dxgi.dll`. Do not edit them
by hand:

```
python tools\gen_dxgi_exports.py
```

Every build compares the new `dxgi.dll` with the local `System32\dxgi.dll` and warns when Windows has added exports.

### Releasing

1. On a Windows PC, in a fresh clone, create an annotated tag on `main` whose message is the release notes:
   `git tag -a vX.Y.Z -F notes.txt`. Git drops every line that starts with `#` from a tag message, so no Markdown
   headings.
2. Check out the tag, build, and run every test above.
3. `python tools\package_release.py vX.Y.Z` checks the tree is exactly at the tag and both DLLs are stamped with it,
   then writes `dist\vX.Y.Z\Banana-Zero-vX.Y.Z.zip` and `SHA256SUMS.txt`.
4. Branch `release/vX.Y.Z` from the tag, commit only `dist\vX.Y.Z\`, push the tag, then push the branch.
5. `.github/workflows/release.yml` verifies the package again and publishes the release. It builds nothing: what ships
   is exactly what was built and tested on the PC.

### Source map

| Path | What |
|---|---|
| `src/main.cpp` | `DllMain`: loads the real `dxgi.dll`, fills the forwarding table |
| `src/ngx_hook.*` | The five NGX hooks |
| `src/nr_dx12.*`, `src/nr_frame.h` | The NR pass: bridge, model, parameters, build and rebuild, encode, model, composite |
| `src/nr.hlsl`, `src/nr_stats.hlsl`, `src/nr_common.hlsli`, `src/nr_shared.h` | The compute shaders and the maths they share |
| `src/settings*` | `dlssnr.ini`: reading, hot reload, the menu's write-back |
| `src/menu*`, `src/overlay_dx12.*`, `src/keys.*`, `src/freeze.*` | The menu, drawn with Dear ImGui on the game's swap chain |
| `src/dxgi.def`, `src/dxgi_stubs.asm`, `src/dxgi_exports.h` | Generated export forwarding |
| `bridge/` | `banana.nvngx.dll` |
| `tests/` | The test programs above, plus a fake NGX runtime and a fake model |
| `tools/` | Build helpers, packaging, and `bzstats.py` / `bzdump.py` for the measurement output |
| `third_party/imgui/` | Dear ImGui v1.92.9b, unmodified |
| `sdk/` | NVIDIA NGX SDK headers |

## Help wanted

Banana-Zero was built by one enthusiast and a lot of patience. Things we would love help with:

- **More games.** Try any DX12 game with DLSS on an RTX 50 card and open an issue with the game, your folder and both
  log files, whether it worked or not.
- **Vulkan support.** The obvious next step. The NGX calls look the same; the plumbing does not.
- **Tuning.** Since v0.1.2 the model's knobs actually do something. What, exactly? Style, Local structure, Local tone
  and friends are undocumented, so screenshots with A/B pairs are gold.
- **HDR know-how.** The white point and shoulder defaults come from two playtests in two games. `StatsLog` and
  `DumpEvery` plus the two Python tools give you the numbers to argue with.
- **Frame time.** A proper measurement of this build, and ideas to make the non-model part cheaper without touching
  image quality.
- **Gamepad users.** The menu does not block the gamepad yet.

Pull requests are welcome. Please run the tests before sending one, and say what you tested on.

## License and fine print

Banana-Zero's code is MIT licensed, see [`LICENSE`](LICENSE). `dxgi.dll` includes Dear ImGui (MIT); see
[`THIRD_PARTY_NOTICES.txt`](THIRD_PARTY_NOTICES.txt).

Banana-Zero is an unofficial hobby project. It is not affiliated with, endorsed by or supported by NVIDIA. NVIDIA,
GeForce, RTX and DLSS are trademarks of NVIDIA Corporation. `nvngx_dlssnr.dll` belongs to NVIDIA and is not
distributed here. Using injected DLLs in games with online components or anti-cheat is at your own risk.

No bananas were harmed in the making of this software.
