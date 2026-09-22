<p align="center">

&#x20; <img src="assets/logo.png" width="600" alt="Sonic the Fighters Recompiled"/>

</p>



<h1 align="center">Sonic the Fighters Recompiled</h1>



<p align="center">

&#x20; A native Windows recompilation of the Xbox 360 XBLA version of <i>Sonic the Fighters</i>.

</p>



<p align="center">

&#x20; <b>Current version: v0.1.0</b>

</p>



\---



\*\*Sonic the Fighters Recompiled\*\* is an unofficial native Windows port of the Xbox 360 XBLA version of \*Sonic the Fighters\*, built through recompilation using the RexGlue / RexGL runtime stack.



The project brings the XBLA release to modern PCs without requiring Xbox 360 emulation or Xenia. It includes native Windows execution, configurable resolution and frame rate, keyboard and controller support, an integrated installer, additional settings, save support, and other improvements designed to make the game easy to install and play on modern systems.



> \[!IMPORTANT]

> \*\*Sonic the Fighters Recompiled does not distribute the original game.\*\*

>

> You must provide the required game data from your own legally acquired copy of \*Sonic the Fighters\*. The installer verifies the supplied game file before installation.



\## Why does this project exist?



\*\*Sonic the Fighters Recompiled exists to preserve the XBLA version of the game and give it a native life on PC.\*\*



This is a version of the game I wanted to experience myself for years. Now that making a native PC version has become possible, I want to preserve it and make it easily accessible to everyone without the hassle and overhead of Xbox 360 emulation.



The goal is simple: provide a convenient, native way to experience the XBLA version of \*Sonic the Fighters\* on modern hardware.



\---



\## Table of Contents



\- \[Features](#features)

\- \[System Requirements](#system-requirements)

\- \[Installation](#installation)

\- \[Controls](#controls)

\- \[Graphics and Recompilation Settings](#graphics-and-recompilation-settings)

\- \[Save Data](#save-data)

\- \[Known Issues](#known-issues)

\- \[Roadmap](#roadmap)

\- \[Technical Details](#technical-details)

\- \[FAQ](#faq)

\- \[Credits](#credits)

\- \[Disclaimer](#disclaimer)



\---



\# Features



\## Native Windows Recompilation



\*Sonic the Fighters\* runs natively on Windows through recompilation instead of running the Xbox 360 version through a traditional emulator.



No Xenia installation or Xbox 360 emulation environment is required.



The result is a lightweight PC release with fast startup, low overhead and direct integration with modern PC features.



\## Complete Game Support



The game can be played from beginning to end.



The current version supports:



\- Full game progression

\- Arcade mode

\- All playable characters

\- Original menus

\- Music

\- Sound effects

\- Save data

\- Achievements

\- Intro sequences

\- In-game cinematics and presentation

\- Xbox 360 controller prompts



The local multiplayer functionality is present but has not yet been fully tested.



Original Xbox Live functionality has not been tested and should not be expected to function, as the original Xbox Live service used by the game is no longer available.



\## High Frame Rate Support



The original game can be played at multiple frame rate targets:



\- \*\*30 FPS\*\*

\- \*\*60 FPS\*\*

\- \*\*120 FPS\*\*



The desired frame rate can be selected through the additional recompilation settings.



\## Resolution Options



The following output resolutions are currently available:



\- \*\*1280×720 — 720p\*\*

\- \*\*1920×1080 — 1080p\*\*

\- \*\*2560×1440 — 1440p\*\*



\## Display Modes



The game supports:



\- \*\*Windowed\*\*

\- \*\*Fullscreen\*\*



\## VSync



Vertical synchronization can be configured through the additional recompilation settings.



\## Native Input Support



Both keyboard and XInput-compatible controllers are supported.



Tested controller configurations include:



\- XInput controllers

\- Dual Joy-Con configuration



The game currently uses the original Xbox 360 button prompts regardless of the connected controller.



\## In-Game Keyboard Remapping



Keyboard controls can be changed directly from within the game.



There is no external launcher required for control configuration.



Press \*\*F6\*\* to reset the keyboard bindings to their default configuration.



\## Integrated Installer



Sonic the Fighters Recompiled includes its own installer designed to make setup as straightforward as possible.



The installer:



1\. Allows you to select \*\*English or Russian\*\* as the installer language.

2\. Requests the required original Xbox 360 game file.

3\. Verifies that the supplied file matches the supported version.

4\. Installs the game automatically.

5\. Launches `stfrecompiled.exe` after installation.



After installation, the installer becomes the game's \*\*uninstaller / maintenance utility\*\*, allowing the installation to be removed or reinstalled when necessary.



\---



\# System Requirements



The exact minimum hardware requirements have not yet been established.



Sonic the Fighters Recompiled is currently tested and supported on:



\### Operating System



\- \*\*Windows 10\*\*

\- \*\*Windows 11\*\*



\### Graphics



Hardware requirements are still being tested and will be documented as compatibility testing continues.



> \[!NOTE]

> Linux is not currently supported. Native Linux support is planned for a future release.



\---



\# Installation



\## What You Need



You must own a legally acquired copy of the Xbox 360 XBLA version of \*Sonic the Fighters\* and dump the required game data from your own Xbox 360 storage.



\*\*The project does not provide copyrighted game data.\*\*



The installer expects the following original game file:



```text

17DB4B597093061B64BFC3E0BCB000BFF14EACBB58

```



The supplied file is verified before installation. Modified or incompatible game data will not be accepted.



\## Installing the Game



1\. Download the latest release of \*\*Sonic the Fighters Recompiled\*\* from the project's Releases page.

2\. Extract the release to a directory of your choice.

3\. Run `install.exe`.

4\. Select the installer language:

&#x20;  - English

&#x20;  - Russian

5\. Select the required original Xbox 360 game file.

6\. Wait while the installer verifies and installs the game.

7\. Once installation is complete, the game will automatically launch.



The installed game executable is:



```text

stfrecompiled.exe

```



After installation, the installer also functions as the maintenance utility for the game and can be used to uninstall or reinstall it.



> \[!WARNING]

> Only use game files dumped from a copy of \*Sonic the Fighters\* that you legally own.

>

> Do not download game files from third-party websites.



\---



\# Controls



Keyboard controls are fully remappable from within the game.



The default bindings are:



| Xbox 360 Input | Keyboard / Mouse |

| --- | --- |

| \*\*Y\*\* | \*\*X\*\* |

| \*\*B\*\* | \*\*Right Mouse Button\*\* |

| \*\*X\*\* | \*\*Z\*\* |

| \*\*A\*\* | \*\*Left Mouse Button\*\* |

| \*\*LB\*\* | \*\*Q\*\* |

| \*\*LT\*\* | \*\*C\*\* |

| \*\*RB\*\* | \*\*E\*\* |

| \*\*RT\*\* | \*\*V\*\* |

| \*\*START\*\* | \*\*Enter / Return\*\* |

| \*\*BACK\*\* | \*\*Backspace\*\* |

| \*\*Move Up\*\* | \*\*W\*\* |

| \*\*Move Down\*\* | \*\*S\*\* |

| \*\*Move Left\*\* | \*\*A\*\* |

| \*\*Move Right\*\* | \*\*D\*\* |



Press \*\*F6\*\* to restore the default keyboard configuration.



\### Local Multiplayer



Keyboard and controller input are handled separately, allowing them to represent different players.



However, local multiplayer has \*\*not yet been fully tested\*\* in the current release.



\---



\# Graphics and Recompilation Settings



Sonic the Fighters Recompiled adds additional settings beyond those provided by the original XBLA release.



Currently configurable options include:



\### Resolution



\- 720p

\- 1080p

\- 1440p



\### Display Mode



\- Windowed

\- Fullscreen



\### VSync



\- Configurable



\### Frame Rate



\- 30 FPS

\- 60 FPS

\- 120 FPS



Additional graphics and recompilation options are planned for future versions.



\---



\# Save Data



Save data is stored inside the game installation directory:



```text

userdata\\

```



Back up this directory if you want to manually preserve your save data before modifying or removing the installation.



\---



\# Known Issues



Sonic the Fighters Recompiled v0.1.0 is an early release. Some issues are expected.



\### Recompilation Settings Menu Size



Under some conditions, the recompilation settings menu can appear smaller than required to correctly display all of its contents.



\### Achievements Menu Freeze



Achievements themselves are supported, but selecting \*\*Achievements\*\* from the game's menu may currently cause the game to freeze.



This issue affects access to the achievements menu and does not mean that the underlying achievement functionality is absent.



\### Recompilation Settings Are Not Saved



Settings added specifically for Sonic the Fighters Recompiled currently do not persist after restarting the game.



They must be configured again after relaunching the application.



\### Local Multiplayer



Local multiplayer has not yet undergone complete compatibility testing.



\### Xbox Live



The original Xbox Live functionality has not been verified and should not currently be considered supported.



\### Mod Compatibility



Modding and compatibility with existing modifications have not yet been tested.



\---



\# Roadmap



Sonic the Fighters Recompiled is still under active development.



Future development is planned to explore or implement:



\- \*\*Russian localization\*\*

\- \*\*Native Linux support\*\*

\- \*\*Nintendo Switch `.nro` port\*\*

\- \*\*Modding support\*\*

\- \*\*Additional graphics settings\*\*

\- \*\*Additional recompilation settings\*\*

\- \*\*Anti-aliasing options\*\*

\- \*\*Online multiplayer\*\*

\- \*\*Compatibility and stability fixes\*\*

\- \*\*Improved visual presentation\*\*

\- \*\*General quality-of-life improvements\*\*



> \[!NOTE]

> Roadmap items represent development goals and are not guarantees for a specific release.



\---



\# Technical Details



\## How It Works



\*\*Sonic the Fighters Recompiled\*\* is a native Windows recompilation project built on top of the \*\*RexGlue / RexGL\*\* runtime stack.



The project does not simply package the original game with prebuilt libraries.



The original Xbox 360 executable had to be analyzed, integrated into the recompilation runtime, adapted for Windows and extensively tested in order to make the game boot, render graphics, play audio, accept input, save data and run from beginning to end outside of its original console environment.



Project-specific work includes:



\- Runtime initialization

\- Sonic the Fighters-specific platform integration

\- Input and controller support

\- Keyboard support and remapping

\- Save handling

\- Additional recompilation settings

\- Installer and maintenance tooling

\- Compatibility fixes

\- Debugging

\- Game testing

\- Release packaging



Development originally began using \*\*XenonRecomp\*\* before eventually moving to \*\*RexGlue / RexGL\*\* as the runtime foundation used for the final implementation.



RexGlue / RexGL provides the underlying recompilation and runtime technology, while the \*Sonic the Fighters\*-specific integration, compatibility work and release implementation were developed specifically for Sonic the Fighters Recompiled.



This allows the game to execute on Windows without requiring traditional Xbox 360 CPU and GPU emulation through software such as Xenia.



\---



\# FAQ



\## Is this an emulator?



No.



Sonic the Fighters Recompiled is a native recompilation project. It uses recompilation/runtime technology to run the Xbox 360 game's code in a native PC environment rather than running the entire Xbox 360 through a traditional emulator.



\## Do I need Xenia?



No.



Xenia is not required to install or play Sonic the Fighters Recompiled.



\## Does the project include Sonic the Fighters?



No.



You must provide the required game data from your own legally acquired Xbox 360 copy.



Sonic the Fighters Recompiled does not distribute the original game's copyrighted assets or game data.



\## Which version of Sonic the Fighters is supported?



The project targets the \*\*Xbox 360 XBLA release of Sonic the Fighters\*\*.



The game itself currently contains English-language content.



The Sonic the Fighters Recompiled installer is available in:



\- English

\- Russian



\## Can I play the entire game?



Yes.



The game has been tested from beginning to end, including gameplay, audio, saves, characters, menus and intro sequences.



Some secondary functionality is still undergoing testing.



\## Does local multiplayer work?



The necessary input functionality is present, including separate keyboard and controller input, but local multiplayer has not yet been fully tested.



\## Does Xbox Live work?



Original Xbox Live functionality is not currently considered supported.



Future versions of Sonic the Fighters Recompiled are planned to explore dedicated online multiplayer functionality independent of the original Xbox Live implementation.



\## Are achievements supported?



The game's achievement functionality is present.



However, the current v0.1.0 release has a known issue where selecting the \*\*Achievements\*\* menu can cause the game to freeze.



\## Can I use a PlayStation controller?



Controllers exposed to the game through an XInput-compatible interface may work, but the game currently displays only the original Xbox 360 controller prompts.



\## Can I use Joy-Cons?



A Dual Joy-Con configuration has been tested successfully.



\## Is Linux supported?



Not yet.



Linux support is planned for a future version.



\## Is Nintendo Switch supported?



Not yet.



A native Nintendo Switch `.nro` version is a future development goal.



\## Are mods supported?



Modding has not yet been tested or officially supported.



Official modding support is planned for future development.



\---



\# Credits



\## Sonic the Fighters Recompiled



\### Sparkos The Wolf



\*\*Creator and Lead Developer\*\*



Responsible for the Sonic the Fighters-specific recompilation integration and release implementation, including:



\- Windows platform integration

\- Runtime integration

\- Input and controller support

\- Keyboard support and remapping

\- Save handling

\- Additional recompilation settings

\- Installer tooling

\- Compatibility work

\- Debugging and testing

\- Release packaging



\## Special Thanks



\- \*\*Unleashed Recompiled\*\* — for the inspiration and motivation behind this project.

\- \*\*XenonRecomp\*\* — for the tools and technical groundwork that helped during development.

\- \*\*RexGL / RexGlue\*\* — for providing the convenient recompilation/runtime framework used as the foundation of the final release.



\---



\# Disclaimer



\*\*Sonic the Fighters Recompiled is an unofficial fan-made project and is not affiliated with, endorsed by, sponsored by, or associated with SEGA or its affiliates.\*\*



\*Sonic the Hedgehog\*, \*Sonic the Fighters\*, and all related characters, names, logos, game assets and intellectual property belong to their respective owners.



This project does \*\*not\*\* distribute the original game or its copyrighted assets.



Users are required to provide game data obtained from their own legally acquired copy of \*Sonic the Fighters\*.



The purpose of Sonic the Fighters Recompiled is to provide the technical software necessary to run a legally obtained copy of the Xbox 360 XBLA version natively on supported modern hardware.



\---



<p align="center">

&#x20; <b>Sonic the Fighters Recompiled</b><br/>

&#x20; Created by <b>Sparkos The Wolf</b>

</p>

