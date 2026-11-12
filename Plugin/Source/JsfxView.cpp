/*
  ------------------------------------------------------------------------------
    JsfxView.cpp

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "JsfxView.h"
#include "Theme.h"

#include <jsfx_embed.h>

namespace fxmenet::plugin
{

JsfxView::JsfxView (FxmeNetAudioProcessor& p, std::uint32_t id) : processor (p), slotId (id)
{
    // The script and its files: in the slot's state, as sent to the server.
    bool found = false;
    for (auto& s : processor.getEngine().getSlots())
        if (s.slotId == slotId)
        {
            jsfx::SlotState state;
            if (jsfx::decode (s.state.data(), s.state.size(), state))
            {
                bundle = std::move (state.bundle);
                found = true;
            }
        }
    if (! found)
    {
        error = "the script is not in the slot's state";
        return;
    }
    if (processor.getSampleRate() > 0.0)
        sampleRate = processor.getSampleRate();

    if (! createInstance())
        return;

    int w = 0, h = 0, hz = 0;
    jsfx_embed_gfx_request (sx, &w, &h, &hz);
    setSize (w > 0 ? w : 640, h > 0 ? h : 400);
    setOpaque (true);

    processor.setEditorShown (slotId, true);   // the server starts with a reset
    shown = true;
    startTimerHz (hz > 0 ? juce::jlimit (1, 60, hz) : 30);
}

JsfxView::~JsfxView()
{
    stopTimer();
    if (shown)
        processor.setEditorShown (slotId, false);
    destroyInstance();
}

//==============================================================================
bool JsfxView::createInstance()
{
    destroyInstance();

    folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                 .getChildFile ("fxmenet-jsfx-view-" + juce::Uuid().toString());
    for (auto& f : bundle.files)
    {
        const auto file = folder.getChildFile (f.data ? "Data" : "Effects").getChildFile (juce::String::fromUTF8 (f.path.c_str()));
        if (! file.getParentDirectory().createDirectory() || ! file.replaceWithData (f.bytes.data(), f.bytes.size()))
        {
            error = "cannot write " + file.getFullPathName();
            return false;
        }
    }
    folder.getChildFile ("Effects").createDirectory();
    folder.getChildFile ("Data").createDirectory();

    char message[1024] = {};
    sx = jsfx_embed_create (folder.getFullPathName().toRawUTF8(), bundle.main.c_str(),
                            (int) std::lround (sampleRate), message, (int) sizeof (message));
    if (sx == nullptr)
    {
        error = juce::String::fromUTF8 (message);
        return false;
    }
    jsfx_embed_set_display (sx);
    error = {};
    return true;
}

void JsfxView::destroyInstance()
{
    if (sx != nullptr)
        sx_destroyInstance (sx);
    sx = nullptr;
    vars.clear();
    subscribed.clear();
    if (folder != juce::File())
        folder.deleteRecursively();
    folder = juce::File();
}

//==============================================================================
void JsfxView::apply (const jsfx::SyncFrame& f)
{
    if (f.reset)
    {
        // Everything anew: a fresh instance (its memory all zero, as the
        // server's copy of it now is), the server's variables mapped by name.
        if (! createInstance())
            return;
        vars.clear();
        vars.reserve (f.names.size());
        for (auto& name : f.names)
            vars.push_back (jsfx_embed_var (sx, name.c_str()));
        haveReset = true;
    }
    if (! haveReset || sx == nullptr)
        return;   // frames of an earlier start

    for (auto& v : f.vars)
        if (v.index < vars.size() && vars[v.index] != nullptr)
            *vars[v.index] = v.value;

    for (auto& r : f.ranges)
        if (auto* block = jsfx_embed_mem_block (sx, (int) (r.offset / jsfx::blockItems), true))
            std::copy (r.values.begin(), r.values.end(), block + r.offset % jsfx::blockItems);
}

void JsfxView::subscribeNewBlocks()
{
    std::array<int, 256> used {};
    const int n = jsfx_embed_mem_blocks_used (sx, used.data(), (int) used.size());
    std::vector<std::uint32_t> fresh;
    for (int i = 0; i < juce::jmin (n, (int) used.size()); ++i)
        if (subscribed.insert ((std::uint32_t) used[(std::size_t) i]).second)
            fresh.push_back ((std::uint32_t) used[(std::size_t) i]);
    processor.getEngine().subscribeJsfx (slotId, fresh);
}

void JsfxView::timerCallback()
{
    if (sx == nullptr && ! error.isEmpty())
        return;

    bool overflowed = false;
    for (auto& payload : processor.getEngine().takeJsfxSync (slotId, overflowed))
    {
        jsfx::SyncFrame f;
        if (jsfx::decode (payload.data(), payload.size(), f) && f.slotId == slotId)
        {
            apply (f);
            ++framesReceived;
        }
    }
    if (overflowed)
    {
        haveReset = false;   // until the server's next reset
        processor.getEngine().restartJsfxSync (slotId);
    }

    if (sx == nullptr || getWidth() <= 0 || getHeight() <= 0)
        return;

    drawFrame();
    if (haveReset)
        subscribeNewBlocks();
}

void JsfxView::drawFrame()
{
    // A script that says it handles high density (gfx_ext_retina > 0) draws
    // at the screen's pixels, the others at the component's size.
    const float scale = juce::jmax (1.0f, (float) juce::Component::getApproximateScaleFactorForComponent (this));
    const auto* density = jsfx_embed_var (sx, "gfx_ext_retina");
    retina = density != nullptr && *density > 0.0 && scale > 1.0f;
    const float k = retina ? scale : 1.0f;
    const int w = juce::roundToInt ((float) getWidth() * k), h = juce::roundToInt ((float) getHeight() * k);
    const int mx = mouse.x >= 0 ? juce::roundToInt ((float) mouse.x * k) : -1;
    const int my = mouse.y >= 0 ? juce::roundToInt ((float) mouse.y * k) : -1;

    const unsigned int* pixels = nullptr;
    int span = 0;
    bool flipped = false;
    const int r = jsfx_embed_gfx_frame (sx, w, h, mx, my, juce::roundToInt (k * 256.0f), &pixels, &span, &flipped);
    if (r < 0 || pixels == nullptr)
        return;

    if (! frame.isValid() || frame.getWidth() != w || frame.getHeight() != h)
        frame = juce::Image (juce::Image::ARGB, w, h, false);
    {
        juce::Image::BitmapData out (frame, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < h; ++y)
        {
            const unsigned int* src = pixels + (std::size_t) (flipped ? h - 1 - y : y) * (std::size_t) span;
            auto* dst = reinterpret_cast<juce::uint32*> (out.getLinePointer (y));
            for (int x = 0; x < w; ++x)
                dst[x] = src[x] | 0xff000000u;   // LICE 0xAARRGGBB; the alpha of a frame buffer means nothing
        }
    }
    lastFrameAt = juce::Time::getMillisecondCounter();
    repaint();
}

//==============================================================================
void JsfxView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
    if (frame.isValid())
        g.drawImage (frame, getLocalBounds().toFloat());

    if (! error.isEmpty())
    {
        g.setColour (theme::error);
        g.setFont (juce::Font (juce::FontOptions (13.0f)));
        g.drawFittedText ("This script's display cannot run here: " + error, getLocalBounds().reduced (12),
                          juce::Justification::centred, 4);
    }
}

void JsfxView::mouseMove (const juce::MouseEvent& e)   { mouse = e.getPosition(); }
void JsfxView::mouseExit (const juce::MouseEvent&)     { mouse = { -1, -1 }; }

} // namespace fxmenet::plugin
