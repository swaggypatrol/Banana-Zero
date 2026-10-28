# DLSSNR 310.9 compatibility (local, unreleased)

This adaptation keeps support for model 310.8.0.0 and adds a known-version profile for 310.9.16.0. It does not ship
NVIDIA's model DLL, modify it, replace DLSS Super Resolution, or enable other GPU architectures.

## Selecting weights

| DLL | Request 0 / default | Request 1 | Request 2 | Other requests |
|---|---|---|---|---|
| 310.8.0.0 | HT (1) | HT (1) | falls back to HT (1) | falls back to HT (1) |
| 310.9.16.0 | CC Small FP8 (2) | HT (1) | CC Small FP8 (2) | falls back to CC Small FP8 (2) |

The new DLL retains the old `WEIGHTS_HT` resource byte-for-byte and adds `WEIGHTS_CC_SMALL_FP8`. This does not promise
that HT produces identical pictures across DLL versions, since its execution code also changed. The internal name
`hnet-crazy-cuckoo-small-fp8` has a separate display alias; neither name is treated as a verified parameter count.

Open the dedicated **Model** tab for the weights dropdown, automatic-mask choice, skin/strength sliders,
and a Reset model controls button. No pre-supplied or hand-edited INI is needed: menu choices apply immediately and
are saved automatically. The new controls are no longer hidden under Advanced. DLLs without the matching content
(310.8, or an unknown build lacking the weight resources/key) show those choices disabled with explanations. The raw INI `Preset` key is still available for users who prefer files. Changing it now requests a feature
rebuild, not a per-evaluation weight change. Same-size pending switches keep the old model running until the existing
30-evaluation cooldown expires; geometry changes wait instead. Old handles/textures use the existing delayed retirement
path. Rapid switching can temporarily increase VRAM, and the existing 64-creation safety limit still applies. Reaching
that limit disables NR for the process rather than exhausting more driver slots. Other model sliders do not rebuild.

Removing `Preset` or right-clicking Model weights explicitly sends 0 at the next creation: it no longer leaves a
previous preset retained in a reused parameter block. Log/status resolution is based on the known version profile,
not a new runtime query claiming to inspect the actual active neural network.

## MaskBlendMode

`MaskBlendMode` is a per-evaluation setting in the new DLL. It is sent with the NGX `int` overload, valid values 0..3,
and written every frame, so a previous value never lingers in the parameter block. Invalid INI values are rejected
without changing a previous valid setting.

In the menu it is **Combine**, on the Depth page under the sky sliders, next to Auto mask and Skin structure (the same
settings as on the Model page), because it only matters with those. Not set (the default, also `MaskBlendMode = auto`)
is **Auto**: Banana-Zero picks the mode each frame from the sliders (`ModelMaskBlendResolve` in `src/model_config.h`):

| Sky local structure vs 1 / Skin structure vs Local structure | Auto sends | Why |
|---|---|---|
| no sky mask, or Auto mask off | 0 | nothing to combine; every mode gives the same picture (T11) |
| both at normal | 2 | nothing to resolve; nonzero so Auto mask stays on |
| down / normal in any mix, nothing up | 2 (keep the lower) | off the sky the sky mask gives Local structure, so only min keeps a lowered Skin structure; on the sky only min keeps a lowered Sky structure |
| up / normal in any mix, nothing down | 3 (keep the higher) | the same with max |
| one up, one down | 1 (average) | no mode keeps both; each gets half |

A line under the dropdown says what is in use and why, with the numbers. A mode chosen by hand (Sky mask only / Average /
Keep the lower / Keep the higher = 0..3) wins over Auto. A log line is written whenever the mode sent changes.

- Mode 0 preserves the legacy rule: an external control mask disables the model's automatic mask. Auto never sends 0
  while the auto mask is on with a sky mask, so it never takes the auto mask away.
- Modes 1..3 no longer force automatic mask off solely because an external mask exists. AutoMask must still be enabled
  for Skin structure to act.
- What the modes do (pre-block kernels `cc_tinlayout_fused_pre_block_swin_1h_32_1[_ds]_fp8`, parameter block 264 -> 272
  bytes, read from the PTX the DLL carries): with an external mask, a nonzero mode and the automatic mask on, the
  two structure strengths handed to the network (skin, and everything else; both -1 with the automatic mask off) become
  `avg` (1), `min` (2) or `max` (3) of themselves and the external mask's `B x LocalStructure`. Banana-Zero's sky mask
  puts Sky structure in B, so: 1 averages, 2 lets the sky mask only lower structure, 3 only raise it. The menu names
  them Average / Keep the lower / Keep the higher. This is a change to the network's input mask, not a blend of
  finished pixels.
- Confirmed on an RTX 5090 with `nrprobe --mask-tests` for both presets: with the automatic mask off or without an
  external mask, modes 1..3 give mode 0's picture exactly (0.000000); with both, each mode differs from mode 0 and min
  differs from max.
- The new DLL detects control/mode changes itself and resets temporal history. These controls never rebuild the feature.
- The parameter is not sent to DLLs without the key (310.8), and their UI retains the legacy mask restriction.

With Auto or a nonzero mode, the sky sliders and AutoMask/Skin structure can be used together. Auto differs from the old
behavior only where the old rule silently discarded the auto mask (a sky slider in use with Auto mask on); everywhere else
it sends 0. This is not a claim that every combination looks good on real faces or in every game.

## What stays unchanged

The six-entry D3D12 bridge and its no-tail-call behavior remain intact. Per-frame controls go directly into NGX parameter
objects, not the old runtime callback that the new model no longer polls. ModelScale still uses smaller Color/Output
subrects and Banana-Zero's own guided fit. It does not depend on the DLL's internal ScalingRatio/guided-upsample branch.

Capabilities come from the file alone, without running NVIDIA code: the PE fixed file version picks the two tested
profiles (310.8.0.0, 310.9.16.0); for any other version the file is mapped as an image resource and checked for the
`RT_RCDATA` weight resources `WEIGHTS_HT` / `WEIGHTS_CC_SMALL_FP8` and for the key string `DLSSNR.MaskBlendMode` in its
read-only data. A later build that still carries them (say 310.9.20) gets the weights choice and Combine, marked
unverified in the menu and log, instead of silently losing them because its version number moved; one that does not
falls back to the DLL defaults. The 310.8 profile is never widened by content. This is capability selection, not
signature verification or a security boundary. Tested by patching only the version resource of a 310.9.16.0 copy to
310.9.17.0: detected from content, `--mask-tests` and `--preset-tests` pass. No
new requirement to change the caller's app ID or SDK version was found or needed in the local standalone tests.

## Fixes included

- Preset changes are included in the feature creation key; ordinary tunables are not. For a known build the key is the
  resolved preset, so requests that pick the same weights (310.9.16.0: 0 and 2; 310.8: all) neither rebuild nor spend
  one of the 64 creations.
- Explicit preset/default and blend/default writes avoid stale NGX parameter values.
- Slider values use readable shortest-round-trip float formatting, rather than four significant digits that could
  change values such as 1.0625 after a restart.
- Invalid Style values no longer mutate the published settings snapshot before validation fails.
- Tunable combo indexing no longer wraps a large unsigned INI value into a negative signed index.
- The capability-block creation fallback cannot exceed the 64-create safety limit.
- Menu tests use a process-specific temporary directory so simultaneous Debug/Release runs cannot corrupt each other's INI/log fixtures.
- `nrprobe` now fails for nonfinite/unreadable basic output and failed release/shutdown results. T9 subrect violations,
  nonfinite output and auxiliary-feature comparison failures also affect its exit code instead of being informational only.

## Reproducing the tests

Build the solution in Release or Debug x64, then run from the repository root:

```text
build\Release\modeltest.exe
build\Release\ngxtest.exe
build\Release\menutest.exe --no-input
build\Release\dxgitest.exe
build\Release\shadertest.exe
```

Optional `modeltest old-dll new-dll` reads both fixture versions without executing the model DLLs. The `--no-input` menu
suite does not send keyboard/mouse events; omit it to run the original two device-input suites as well.

Real-model tests (supply your own paths):

```text
build\Release\nrprobe.exe --model <old-dll> --preset 1 --size 512x512 --evaluates 5
build\Release\nrprobe.exe --model <new-dll> --preset 1 --size 512x512 --evaluates 5 --mask-tests --preset-tests
build\Release\nrprobe.exe --model <new-dll> --preset 2 --size 1920x1080 --tuning --subrects --mask-tests
```

`--preset-tests` (T12) creates a feature per request 0, 1 and 2 against the main one: on 310.9.16.0 request 0 and 2
give the identical picture and 1 differs (mean 0.0118 at 512x512), so the `int` preset key demonstrably reaches the
model. All auxiliary features use the same requested preset, including the capability fallback and the features used
for subrect comparisons. `--mask-blend N` selects a baseline mode; `--mask-tests` checks all four modes with AutoMask on/off
and with/without an external mask, 16 combinations per preset. They check finite output for all 16, then the modes' behaviour as above. Basic and T9 checks use exit status as well as printed output; inspect both the final
verdict and teardown.

## Local validation and remaining gaps

The latest Release and Debug no-input menu suites each pass 226 checks, including opening the actual dropdowns,
selecting every preset and Combine option (Auto and the four by hand), starting without an INI, automatic file creation, closing/reloading, exact slider
value persistence and the reset-model button. These are headless interactions with the actual panel, not assignments
standing in for dropdown clicks.

Initial local runs on an RTX 5090 with driver 621.01 passed the CPU/model/bridge/menu suites, the DXGI/shader suites,
old-model creation/evaluation/shutdown, new-model presets 1 and 2, all 16 mask combinations for each new preset, and
1920x1080 subrect tests down to 33.3%. T9 observed no writes outside the subrect and zero reported mean difference against
an auxiliary feature created at that size. The new FP8 tuning run had no mandatory tuning-check failures.

T10's motion-history result for FP8 was **inconclusive** on the synthetic frame: history barely improved it. No new
motion-vector conversion was inferred from that. Real-game frame-dump replay was skipped when no explicit test fixtures
were supplied. No per-game visual judgment, long-session in-game preset switching or cross-driver qualification has
been completed. The two original device-input suites were not exercised by the local `--no-input` run. These remain
release-validation tasks; standalone tests are not a blanket compatibility guarantee.

No GitHub push, PR or release is part of this local adaptation.
