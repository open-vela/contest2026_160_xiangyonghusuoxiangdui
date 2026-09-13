---
name: board-change-tracker
description: >-
  Use when adapting, bringing up, or developing support for a hardware board in
  this Armbian build framework (e.g. adding a config/boards/*.conf, editing a
  family under config/sources/families/, adding kernel/u-boot patches, tweaking
  kernel/u-boot defconfigs, wiring rkbin blobs, or changing build parameters).
  Records every change to a per-board changelog so nothing is missed, and drives
  a single, complete commit at the end of the work so no adaptation change is
  left uncommitted.
---

# Board Change Tracker

Track every change made while adapting or developing a board, then commit them
all at the end. The goal: **no board-adaptation change ever gets forgotten or
left uncommitted.**

## When to use

Activate this skill whenever the task involves board bring-up / adaptation /
development in this repo. Typical triggers:

- Creating or editing `config/boards/<board>.conf` (or `.csc`)
- Editing a family file under `config/sources/families/` (incl. `include/*.inc`)
- Adding or changing patches under `patch/kernel/**` or `patch/u-boot/**`
- Changing a kernel config (`config/kernel/*.config`) or u-boot defconfig
- Wiring rkbin blobs (`DDR_BLOB`, `BL31_BLOB`, `BL32_BLOB`, etc.)
- Adding files under `userpatches/**`
- Recording the working build/flash commands and parameters (BRANCH, RELEASE,
  BOOT_SCENARIO, BOOTCONFIG, boot media, serial baud, etc.)

## Where the log lives

Maintain one changelog per board:

```
.kiro/board-changes/<board-name>.md
```

Use the `BOARD` name (kebab-case, same as the `config/boards/<board>.conf`
filename without extension), e.g. `.kiro/board-changes/rk3588-evb7-v11.md`.

If the file does not exist, create it from the template in the next section.

## Tracking process (do this AFTER every change)

Immediately after making or discovering a board-related change, append an entry
to the board's changelog. Do not batch this — record as you go so nothing is
lost across context compaction or long sessions.

Each entry must capture:

- **What** — the exact file path(s) touched (or command run)
- **Type** — one of: board-config, family, kernel-patch, uboot-patch,
  kernel-config, uboot-config, rkbin, userpatch, build-param, doc, other
- **Change** — a one-line description of what was added/edited/removed
- **Why** — the rationale (why this was needed for the board)

Also keep a short "Build & flash" section with the known-good commands and the
key parameters (BRANCH, RELEASE, BOOTCONFIG, BOOT_SCENARIO, boot media, serial
baud) so the setup is reproducible.

### Changelog template

```markdown
# Board adaptation log: <board-name>

- SoC / family: <e.g. RK3588 / rockchip-rk3588>
- Kernel branch: <e.g. vendor (6.1)>
- Status: <in-progress | booting | complete>

## Build & flash (known-good)
- Build: `./compile.sh build BOARD=<board> BRANCH=<branch> RELEASE=<rel> BUILD_MINIMAL=yes BUILD_DESKTOP=no`
- Flash: <sd dd / rkdeveloptool wl 0 ... via MASKROM>
- Serial: <device @ baud, e.g. UART2 @ 1500000, console=ttyFIQ0>
- Boot media: <sd | emmc | spi | nvme>

## Changes
| # | File / command | Type | Change | Why |
|---|----------------|------|--------|-----|
| 1 | config/boards/<board>.conf | board-config | created | new board definition |

## Open items / TODO
- <anything not yet done or uncertain>
```

## Final commit checklist (run at project end)

When the board work is done (or the user asks to commit / wrap up):

1. Read the board changelog and build the expected list of touched paths.
2. Run `git status --porcelain` and `git diff --stat` to see actual changes.
3. **Cross-check**: every changed file should be explained by a changelog entry;
   every changelog entry should map to a real change. Reconcile any mismatch:
   - Untracked/modified file with no log entry → investigate and add an entry
     (or discard if unintended).
   - Log entry with no corresponding change → note it (maybe reverted).
4. Stage only the board-adaptation files (avoid `git add .`; stage explicit
   paths). Flag anything that looks like a secret or unrelated change.
5. Commit with a structured message, e.g.:
   ```
   board: add <board-name> support

   - config/boards/<board>.conf: <what>
   - <other paths>: <what>

   Kernel: <branch/version>  U-Boot: <source/defconfig>
   Verified: <boots via serial / flashed to emmc>, etc.
   ```
6. Report to the user: the commit hash, the files included, and explicitly list
   anything intentionally left out.

## Rules

- Never `git add .` blindly — stage explicit, reviewed paths.
- Do not create commits unless the user asked to wrap up / commit, or confirms.
- Keep the changelog in sync in real time; treat it as the source of truth for
  "what did we change for this board".
- If the repo already had uncommitted changes before this work started, note
  them separately so they are not attributed to the board adaptation.
