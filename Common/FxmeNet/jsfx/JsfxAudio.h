/*
  ------------------------------------------------------------------------------
    JsfxAudio.h

    Audio files for JSFX file sliders and file_open() (doc/jsfx-plan.md). The
    engine reads 8, 16 and 24-bit PCM .wav, mono or stereo, itself; REAPER
    reads the rest with its own media API, which FxmeNet does not have. So
    the client converts what the engine cannot read (FLAC, AIFF, Ogg, MP3,
    floating-point or 32-bit .wav) to 24-bit .wav while packing a script
    (jsfx::BundleBuilder::convertAudio), with JUCE's readers.

    Needs juce_audio_formats. Header-only, as the rest of Common's JUCE code.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace fxmenet::jsfx
{

/** True, with the file as a 24-bit .wav in `wav`, when `file` is audio the
    engine cannot read as it is. False for anything else (sent unchanged):
    not audio, already readable, more than two channels, or not decodable. */
inline bool convertAudioForJsfx (const juce::File& file, juce::MemoryBlock& wav)
{
    const auto ext = file.getFileExtension().toLowerCase();
    if (! juce::StringArray { ".wav", ".wave", ".aif", ".aiff", ".flac", ".ogg", ".mp3" }.contains (ext))
        return false;

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->numChannels < 1 || reader->numChannels > 2 || reader->lengthInSamples <= 0)
        return false;
    if ((ext == ".wav" || ext == ".wave") && ! reader->usesFloatingPointData && reader->bitsPerSample <= 24)
        return false;   // the engine reads it

    wav.reset();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (wav, false);
    juce::WavAudioFormat format;
    auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                      .withSampleRate (reader->sampleRate)
                                                      .withNumChannels ((int) reader->numChannels)
                                                      .withBitsPerSample (24));
    if (writer == nullptr || ! writer->writeFromAudioReader (*reader, 0, -1))
        return false;
    writer.reset();   // writes the header, and lets go of the stream
    return wav.getSize() > 44;
}

} // namespace fxmenet::jsfx
