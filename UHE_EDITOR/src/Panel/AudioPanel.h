#pragma once

namespace UHE
{
// Mixer view over the live voices: master volume, per-voice volume/pitch,
// loop and stop, plus a quick 2D file preview. Values here operate on the
// actual miniaudio voices, not a copy.
class AudioPanel
{
public:
    void OnImGuiRender(bool& open);
};
} // namespace UHE
