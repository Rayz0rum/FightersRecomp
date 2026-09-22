<div align="center">

<img src="assets/logo.png" width="600" alt="Sonic the Fighters Recompiled">

# Sonic the Fighters Recompiled

Native Windows recompilation of the Xbox 360 XBLA version of *Sonic the Fighters*.

**Current version: v0.1.0**

</div>

---

## About

**Sonic the Fighters Recompiled** is an unofficial native Windows port of the Xbox 360 XBLA release of *Sonic the Fighters*.

The project uses RexGlue / RexGL to run the game natively on PC without Xenia or a full Xbox 360 emulation environment.

I started this project because I wanted a proper PC version of the XBLA release. Development originally started with XenonRecomp, but I later moved to RexGlue / RexGL, which became the foundation of the current version.

The original game is **not included**. You need your own legally acquired Xbox 360 copy of *Sonic the Fighters*.

> [!IMPORTANT]
> `rexruntime.dll` may be detected by some antivirus software. This is a false positive. The DLL comes from the original RexGlue SDK and can be replaced with the version from its repository.
> AI used in images of installer, some description information. 


---

## Features

- Native Windows executable
- Full game playable from start to finish
- Keyboard and XInput controller support
- Keyboard remapping
- Save support
- Achievements
- 720p, 1080p and 1440p output
- 30, 60 and 120 FPS options
- Windowed and fullscreen modes
- VSync option
- Integrated installer
- English and Russian installer languages
- Original menus, music, sound effects and cinematics

The game currently uses the original Xbox 360 button prompts.

A Dual Joy-Con configuration has also been tested successfully.

---

## Requirements

### Operating system

- Windows 10
- Windows 11

Exact minimum hardware requirements have not been established yet.

Linux is not supported in the current release.

---

## Installation

You need a legally acquired copy of the Xbox 360 XBLA version of *Sonic the Fighters*.

The installer expects the following game file:

```text
17DB4B597093061B64BFC3E0BCB000BFF14EACBB58
```

To install:

1. Download and extract the latest release.
2. Run `install.exe`.
3. Select English or Russian.
4. Select the required game file from your own dump.
5. Wait for verification and installation.

The installer will launch `stfrecompiled.exe` when installation is finished.

After that, `install.exe` can also be used to uninstall or reinstall the game.

Modified or unsupported game files will fail verification.

---

## Controls

| Xbox 360 | Keyboard / Mouse |
| --- | --- |
| Y | X |
| B | Right Mouse Button |
| X | Z |
| A | Left Mouse Button |
| LB | Q |
| LT | C |
| RB | E |
| RT | V |
| START | Enter / Return |
| BACK | Backspace |
| Up | W |
| Down | S |
| Left | A |
| Right | D |

Keyboard bindings can be changed in-game.

Press **F6** to restore the default bindings.

---

## Save Data

Save files are stored in:

```text
userdata\
```

Back up this folder if you want to keep your saves before removing the game.

---

## Known Issues

This is an early release, so there are still some problems to fix.

- The recompilation settings menu may sometimes be too small for its contents.
- Opening the **Achievements** menu can freeze the game. Achievement functionality itself is present.
- Recompilation-specific settings are currently reset after restarting the game.
- Local multiplayer input is present but has not been fully tested.
- Original Xbox Live functionality is not considered supported.
- Mod compatibility has not been tested yet.

---

## Roadmap

At this moment I'm working on
<b>new installer, new in-game windows</b>
progress: ■■■■■□□□□□ 50%

Things I would like to work on in future versions:

- Russian localization
- Linux support
- Nintendo Switch `.nro` port
- Modding support
- More graphics and recompilation settings
- Anti-aliasing options
- Online multiplayer
- Compatibility and stability fixes
- General quality-of-life improvements

These are development goals, not promises for a particular release.

---

## Technical Details

The project is built on the **RexGlue / RexGL** runtime stack.

RexGlue / RexGL provides the underlying recompilation and runtime technology. The work on this project is focused on getting *Sonic the Fighters* working correctly with that runtime and turning it into a usable Windows release.

This included work on platform and runtime integration, input, keyboard remapping, saves, additional settings, compatibility fixes, debugging, testing, the installer and release packaging.

Development originally started with **XenonRecomp** before moving to RexGlue / RexGL.

This is not an emulator package and does not require Xenia to run.

---

## FAQ

### Does this include the original game?

No. You have to provide the required file from your own legally acquired Xbox 360 copy.

### Which version is supported?

The Xbox 360 XBLA release of *Sonic the Fighters*.

### Can the whole game be completed?

Yes. The game has been tested from beginning to end.

### Can I use a PlayStation controller?

It may work if it is exposed to the game as an XInput-compatible controller. The game will still display Xbox 360 button prompts.

### Is the game itself translated into Russian?

Not yet. The installer supports Russian, but the game currently remains in English.

---

## Credits

### Sparkos The Wolf

Creator and Lead Developer of **Sonic the Fighters Recompiled**.

### Special Thanks

- **[Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp)** — inspiration for this project.
- **[XenonRecomp](https://github.com/hedge-dev/XenonRecomp)** — tools and technical groundwork used during early development.
- **[RexGL / RexGlue](https://github.com/rexglue/rexglue-sdk)** — recompilation/runtime framework used by the current version.

---

## Disclaimer

**Sonic the Fighters Recompiled** is an unofficial fan project and is not affiliated with or endorsed by SEGA.

*Sonic the Hedgehog*, *Sonic the Fighters* and all related characters, names, logos, assets and intellectual property belong to their respective owners.

This project does not distribute the original game or its copyrighted game data.

---

<div align="center">

<b>Sonic the Fighters Recompiled</b><br>
Created by <b>Sparkos The Wolf</b>

</div>
