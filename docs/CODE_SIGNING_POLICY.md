# Code signing policy

**Status:** Banana-Zero has applied to the [SignPath Foundation](https://signpath.org) for free code signing of its
releases. Until that is set up, release DLLs are not signed. Once it is, this page and the README will say:
"Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by
[SignPath Foundation](https://signpath.org)".

## What is signed

Only Banana-Zero's own binaries, built from this repository:

- `dxgi.dll`
- `banana.nvngx.dll`

They are built by the `build` workflow (`.github/workflows/build.yml`) on a GitHub-hosted Windows machine, from the
tagged commit of a release, with no manual step between the source and the binary. The build stamps both DLLs with
`git describe` of that commit, and the workflow checks the stamp. Before a release is approved for signing, the same
build is tested on an NVIDIA RTX 50 PC (the GPU tests in the README) and played in the tested games.

Nothing else is signed. In particular NVIDIA's `nvngx_dlssnr.dll`, which users supply themselves, is not part of the
release and is never signed by this project.

## Team roles

| Role | Members |
|---|---|
| Committers and reviewers | [swaggypatrol](https://github.com/swaggypatrol) |
| Approvers | [swaggypatrol](https://github.com/swaggypatrol) |

Changes reach `main` only through pull requests in this repository. Much of the code is written by AI coding
assistants working for the maintainer, in the maintainer's account. Contributions from anyone else are reviewed by a
reviewer before they are merged. Every signing request is approved by hand by an approver. All members must use
multi-factor authentication for GitHub and SignPath.

## Privacy

This program will not transfer any information to other networked systems unless specifically requested by the user
or the person installing or operating it. Banana-Zero contains no networking code. It writes only its log files
(`dlssnr.log`, `dlssnr.prev.log`) and settings file (`dlssnr.ini`) in the game's folder, and the NR model's own data
and, when asked for, frame dumps under `%LOCALAPPDATA%\Banana-Zero`.

## Installing and removing

Banana-Zero has no installer and changes no system setting. It is installed by copying its two DLLs into a game's
folder, and removed by deleting them along with the files named above (README, "Installation").
