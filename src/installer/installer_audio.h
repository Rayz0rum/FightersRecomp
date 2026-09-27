// The installer's music and sounds (the game's own, HCA), played with SDL -
// the runtime's audio system doesn't exist yet while installing.
#pragma once

#include "installer_common.h"

namespace InstallerAudio {
void Init();
void Shutdown();
// Starts the looping music (once decoded), or keeps it playing.
void PlayMusic();
void FadeOutMusic();
void Play(InstallerSound sound);
}  // namespace InstallerAudio
