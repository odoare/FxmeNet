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

#include <cctype>
#include <cstring>

namespace fxmenet::plugin
{

namespace
{
    /** The variables assigned in the @sample and @block sections of the
        script's code files (lower case: EEL names ignore case). A lexical
        guess: `name =`, `name += ...`, not memory (`name[...] =`), not what
        functions called from there assign. */
    std::set<std::string> audioAssigned (const jsfx::Bundle& bundle)
    {
        std::set<std::string> out;
        for (auto& f : bundle.files)
        {
            const auto ext = juce::File (juce::String::fromUTF8 (f.path.c_str())).getFileExtension().toLowerCase();
            if (f.data || ! (ext.isEmpty() || ext == ".jsfx" || ext == ".jsfx-inc" || ext == ".eel"))
                continue;
            const std::string text (f.bytes.begin(), f.bytes.end());
            const auto n = text.size();
            bool audio = false, lineStart = true;
            for (std::size_t i = 0; i < n;)
            {
                const char c = text[i];
                if (lineStart && c == '@')
                {
                    std::size_t e = i + 1;
                    while (e < n && (std::isalnum ((unsigned char) text[e]) || text[e] == '_'))
                        ++e;
                    const auto section = text.substr (i + 1, e - i - 1);
                    audio = section == "sample" || section == "block";
                    i = e;
                    lineStart = false;
                    continue;
                }
                if (c == '\n')                          { lineStart = true; ++i; continue; }
                if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
                lineStart = false;
                if (c == '/' && i + 1 < n && text[i + 1] == '/')
                {
                    while (i < n && text[i] != '\n')
                        ++i;
                    continue;
                }
                if (c == '/' && i + 1 < n && text[i + 1] == '*')
                {
                    const auto e = text.find ("*/", i + 2);
                    i = e == std::string::npos ? n : e + 2;
                    continue;
                }
                if (c == '"' || c == '\'')
                {
                    std::size_t e = i + 1;
                    while (e < n && text[e] != c)
                        e += text[e] == '\\' ? 2 : 1;
                    i = e + 1;
                    continue;
                }
                if (std::isalpha ((unsigned char) c) || c == '_')
                {
                    std::size_t e = i + 1;
                    while (e < n && (std::isalnum ((unsigned char) text[e]) || text[e] == '_' || text[e] == '.'))
                        ++e;
                    auto name = text.substr (i, e - i);
                    i = e;
                    if (! audio)
                        continue;
                    std::size_t k = e;
                    while (k < n && (text[k] == ' ' || text[k] == '\t'))
                        ++k;
                    const bool plain = k < n && text[k] == '=' && (k + 1 >= n || text[k + 1] != '=');
                    const bool compound = k + 1 < n && text[k + 1] == '=' && std::strchr ("+-*/%|&^~", text[k]) != nullptr;
                    if (plain || compound)
                    {
                        for (auto& ch : name)
                            ch = (char) std::tolower ((unsigned char) ch);
                        out.insert (name);
                    }
                    continue;
                }
                ++i;
            }
        }
        return out;
    }
}

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
    for (auto& f : bundle.files)
        if (! f.data && std::search (f.bytes.begin(), f.bytes.end(), "gmem", "gmem" + 4) != f.bytes.end())
            usesGmem = true;
    if (processor.getSampleRate() > 0.0)
        sampleRate = processor.getSampleRate();

    if (! createInstance())
        return;

    int w = 0, h = 0, hz = 0;
    jsfx_embed_gfx_request (sx, &w, &h, &hz);
    setSize (w > 0 ? w : 640, h > 0 ? h : 400);
    setOpaque (true);
    setWantsKeyboardFocus (true);

    processor.setEditorShown (slotId, true);   // the server starts with a reset
    shown = true;
    startTimerHz (hz > 0 ? juce::jlimit (1, 60, hz) : 30);
}

JsfxView::~JsfxView()
{
    stopTimer();
    for (int p : gestures)
        processor.endSlotParameterGesture ({ slotId, (std::uint32_t) p });
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
    jsfx_embed_gfx_host host { this, &JsfxView::showMenuHook, &JsfxView::setCursorHook };
    jsfx_embed_set_gfx_host (sx, &host);
    error = {};
    return true;
}

void JsfxView::destroyInstance()
{
    if (sx != nullptr)
    {
        jsfx_embed_set_gfx_host (sx, nullptr);
        sx_destroyInstance (sx);
    }
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

        // What may go back: not the display's own state (gfx_*, mouse_*),
        // not the sliders (they go as parameters).
        sliderVars.clear();
        for (int p = 0, n = sx_getNumParms (sx); p < n; ++p)
            sliderVars.push_back (jsfx_embed_slider_var (sx, p));
        writable.assign (vars.size(), true);
        for (std::size_t i = 0; i < vars.size(); ++i)
        {
            const auto& name = f.names[i];
            writable[i] = vars[i] != nullptr
                       && name.rfind ("gfx_", 0) != 0 && name.rfind ("mouse_", 0) != 0
                       && std::find (sliderVars.begin(), sliderVars.end(), vars[i]) == sliderVars.end();
        }
        remote.assign (vars.size(), 0.0);
        audioOwned.assign (vars.size(), false);
        const auto assigned = audioAssigned (bundle);
        for (std::size_t i = 0; i < vars.size(); ++i)
            audioOwned[i] = writable[i] && assigned.count (juce::String (f.names[i]).toLowerCase().toStdString()) > 0;
        haveReset = true;
    }
    if (! haveReset || sx == nullptr)
        return;   // frames of an earlier start

    for (auto& v : f.vars)
        if (v.index < vars.size() && vars[v.index] != nullptr)
            *vars[v.index] = remote[v.index] = v.value;

    for (auto& r : f.ranges)
        if (auto* block = memoryBlock (r.offset / jsfx::blockItems, true))
            std::copy (r.values.begin(), r.values.end(), block + r.offset % jsfx::blockItems);

    for (auto& [key, text] : f.strings)
        jsfx_embed_set_string (sx, key.c_str(), text.data(), (int) text.size());
}

double* JsfxView::memoryBlock (std::uint32_t block, bool allocate)
{
    return block >= jsfx::gmemBlockBase ? jsfx_embed_gmem_block (sx, (int) (block - jsfx::gmemBlockBase), allocate)
                                        : jsfx_embed_mem_block (sx, (int) block, allocate);
}

void JsfxView::subscribeNewBlocks()
{
    std::array<int, 256> used {};
    std::vector<std::uint32_t> fresh;
    const int n = jsfx_embed_mem_blocks_used (sx, used.data(), (int) used.size());
    for (int i = 0; i < juce::jmin (n, (int) used.size()); ++i)
        if (subscribed.insert ((std::uint32_t) used[(std::size_t) i]).second)
            fresh.push_back ((std::uint32_t) used[(std::size_t) i]);

    // gmem[]: shared by every script of the process, so "allocated" does not
    // mean "this script read it": only for a script that names it.
    if (usesGmem)
    {
        const int g = jsfx_embed_gmem_blocks_used (sx, used.data(), (int) used.size());
        for (int i = 0; i < juce::jmin (g, (int) used.size()); ++i)
        {
            const auto id = jsfx::gmemBlockBase + (std::uint32_t) used[(std::size_t) i];
            if (subscribed.insert (id).second)
                fresh.push_back (id);
        }
    }
    processor.getEngine().subscribeJsfx (slotId, fresh);
}

void JsfxView::timerCallback()
{
    if (inFrame || menuOpen || (sx == nullptr && ! error.isEmpty()))
        return;   // a menu of the script is open: the display waits for the choice
    const juce::ScopedValueSetter<bool> busy (inFrame, true);

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

    if (haveReset)
    {
        // What the audio code keeps setting there (see the header).
        for (std::size_t i = 0; i < vars.size() && i < audioOwned.size(); ++i)
            if (audioOwned[i])
                *vars[i] = remote[i];
        snapshot();
    }
    drawFrame();
    if (haveReset)
    {
        sendChanges();
        subscribeNewBlocks();
    }
    if (menuRequest.isNotEmpty())
        openMenu();
}

//==============================================================================
void JsfxView::snapshot()
{
    sliderBefore.resize (sliderVars.size());
    for (std::size_t p = 0; p < sliderVars.size(); ++p)
        sliderBefore[p] = sliderVars[p] != nullptr ? *sliderVars[p] : 0.0;

    beforeBlocks.clear();
    before.clear();
    beforeStrings.clear();
    if (juce::Time::getMillisecondCounter() - lastInteraction > interactionHoldMs && buttons == 0)
        return;   // nobody is acting on the display: what @gfx changes is its own

    beforeStrings = readStrings();
    before.resize (vars.size());
    for (std::size_t i = 0; i < vars.size(); ++i)
        before[i] = writable[i] ? *vars[i] : 0.0;
    for (auto b : subscribed)
    {
        if (beforeBlocks.size() >= maxSnapshotBlocks)
            break;
        if (const auto* block = memoryBlock (b, false))
            beforeBlocks[b].assign (block, block + jsfx::blockItems);
    }
}

jsfx::Strings JsfxView::readStrings() const
{
    jsfx::Strings out;
    jsfx_embed_enum_strings (sx, [] (void* context, const char* key, const char* value, int length)
    {
        static_cast<jsfx::Strings*> (context)->emplace_back (key, std::string (value, (std::size_t) juce::jmax (0, length)));
    }, &out);
    return out;
}

void JsfxView::sendChanges()
{
    // Sliders: as parameters, normalised as the server's are (JsfxInstance).
    for (std::size_t p = 0; p < sliderVars.size() && p < sliderBefore.size(); ++p)
    {
        if (sliderVars[p] == nullptr || std::memcmp (sliderVars[p], &sliderBefore[p], sizeof (double)) == 0)
            continue;
        double lo = 0.0, hi = 1.0, step = 0.0;
        const double v = sx_getParmVal (sx, (int) p, &lo, &hi, &step);
        const float normalised = std::abs (hi - lo) > 0.0 ? (float) juce::jlimit (0.0, 1.0, (v - lo) / (hi - lo)) : 0.0f;
        const Mapping target { slotId, (std::uint32_t) p };
        if (buttons != 0 && gestures.insert ((int) p).second)
            processor.beginSlotParameterGesture (target);
        processor.setLastTouched (target);
        processor.setSlotParameter (target, normalised);
    }
    if (buttons == 0 && ! gestures.empty())
    {
        for (int p : gestures)
            processor.endSlotParameterGesture ({ slotId, (std::uint32_t) p });
        gestures.clear();
    }

    if (before.empty())
        return;   // not acting on it: nothing else goes back

    writeFrame = {};
    writeFrame.slotId = slotId;
    for (std::size_t i = 0; i < vars.size() && i < before.size(); ++i)
        if (writable[i] && std::memcmp (vars[i], &before[i], sizeof (double)) != 0)
        {
            writeFrame.vars.push_back ({ (std::uint32_t) i, *vars[i] });
            remote[i] = *vars[i];   // the server's value now (it does not echo it)
        }

    for (auto& [b, copy] : beforeBlocks)
    {
        const auto* now = memoryBlock (b, false);
        if (now == nullptr)
            continue;
        jsfx::SyncFrame::Range* open = nullptr;
        for (std::uint32_t c = 0; c < jsfx::blockItems; c += jsfx::chunkItems)
        {
            if (std::memcmp (now + c, copy.data() + c, jsfx::chunkItems * sizeof (double)) == 0)
            {
                open = nullptr;
                continue;
            }
            if (open == nullptr)
            {
                writeFrame.ranges.push_back ({ b * jsfx::blockItems + c, {} });
                open = &writeFrame.ranges.back();
            }
            open->values.insert (open->values.end(), now + c, now + c + jsfx::chunkItems);
        }
    }

    const auto after = readStrings();
    for (auto& entry : after)
        if (std::find (beforeStrings.begin(), beforeStrings.end(), entry) == beforeStrings.end()
            && entry.second.size() <= jsfx::maxStringBytes)
            writeFrame.strings.push_back (entry);

    if (writeFrame.vars.empty() && writeFrame.ranges.empty() && writeFrame.strings.empty())
        return;
    jsfx::encode (writeFrame, writePayload);
    processor.getEngine().writeJsfx (writePayload);
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
    const int cap = capOverride >= 0 ? capOverride : (buttons | modifiers);
    const int r = jsfx_embed_gfx_frame (sx, w, h, mx, my, cap, juce::roundToInt (k * 256.0f), &pixels, &span, &flipped);
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

void JsfxView::touched()   { lastInteraction = juce::Time::getMillisecondCounter(); }

int JsfxView::mouseCap (const juce::ModifierKeys& m) const
{
    int cap = 0;
   #if JUCE_MAC
    if (m.isCommandDown()) cap |= 4;
    if (m.isCtrlDown())    cap |= 32;
   #else
    if (m.isCtrlDown())    cap |= 4;
   #endif
    if (m.isShiftDown())   cap |= 8;
    if (m.isAltDown())     cap |= 16;
    return cap;
}

void JsfxView::mouseMove (const juce::MouseEvent& e)
{
    mouse = e.getPosition();
    modifiers = mouseCap (e.mods);
}

void JsfxView::mouseDrag (const juce::MouseEvent& e)
{
    mouse = e.getPosition();
    modifiers = mouseCap (e.mods);
    touched();
}

void JsfxView::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    mouse = e.getPosition();
    modifiers = mouseCap (e.mods);
    buttons = (e.mods.isLeftButtonDown() ? 1 : 0) | (e.mods.isRightButtonDown() ? 2 : 0) | (e.mods.isMiddleButtonDown() ? 64 : 0);
    touched();
}

void JsfxView::mouseUp (const juce::MouseEvent& e)
{
    mouse = e.getPosition();
    modifiers = mouseCap (e.mods);
    buttons = 0;   // JUCE reports the buttons still down after the one released: none, here
    touched();
}

void JsfxView::mouseExit (const juce::MouseEvent&)
{
    if (buttons == 0)
        mouse = { -1, -1 };
}

void JsfxView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // mouse_wheel counts 120 a notch; JUCE gives about 0.2 a notch.
    if (sx != nullptr)
        jsfx_embed_gfx_wheel (sx, wheel.deltaY * 600.0 * (wheel.isReversed ? -1.0 : 1.0), wheel.deltaX * 600.0);
    mouse = e.getPosition();
    touched();
}

//==============================================================================
namespace
{
    constexpr int chars (const char* s)
    {
        int v = 0;
        for (; *s != 0; ++s)
            v = (v << 8) | (unsigned char) *s;
        return v;
    }
}

int JsfxView::scriptKeyCode (const juce::KeyPress& k)
{
    using K = juce::KeyPress;
    const int code = k.getKeyCode();
    if (code == K::upKey)         return chars ("up");
    if (code == K::downKey)       return chars ("down");
    if (code == K::leftKey)       return chars ("left");
    if (code == K::rightKey)      return chars ("rght");
    if (code == K::homeKey)       return chars ("home");
    if (code == K::endKey)        return chars ("end");
    if (code == K::pageUpKey)     return chars ("pgup");
    if (code == K::pageDownKey)   return chars ("pgdn");
    if (code == K::insertKey)     return chars ("ins");
    if (code == K::deleteKey)     return chars ("del");
    if (code == K::returnKey)     return 13;
    if (code == K::escapeKey)     return 27;
    if (code == K::backspaceKey)  return 8;
    if (code == K::tabKey)        return 9;
    for (int f = 1; f <= 12; ++f)
        if (code == K::F1Key + (f - 1))
            return f < 10 ? chars ("f") << 8 | ('0' + f) : chars ("f1") << 8 | ('0' + f - 10);

    // Control + a letter: 1 to 26.
    if (k.getModifiers().isCtrlDown() && code >= 'A' && code <= 'Z')
        return code - 'A' + 1;
    if (k.getModifiers().isCtrlDown() && code >= 'a' && code <= 'z')
        return code - 'a' + 1;

    const auto c = (int) k.getTextCharacter();
    if (c >= 32 && c < 256)
        return c;
    if (c >= 256)
        return ('u' << 24) | c;
    return code > 0 && code < 128 ? code : 0;
}

bool JsfxView::keyPressed (const juce::KeyPress& k)
{
    const int code = scriptKeyCode (k);
    if (sx == nullptr || code == 0)
        return false;
    jsfx_embed_gfx_key (sx, code, 1);
    heldKeys[code] = k.getKeyCode();
    touched();
    return true;   // the DAW does not get it while the display has the focus
}

bool JsfxView::keyStateChanged (bool isKeyDown)
{
    if (isKeyDown || sx == nullptr)
        return false;
    for (auto it = heldKeys.begin(); it != heldKeys.end();)
    {
        if (! juce::KeyPress::isKeyCurrentlyDown (it->second))
        {
            jsfx_embed_gfx_key (sx, it->first, 0);
            it = heldKeys.erase (it);
        }
        else
            ++it;
    }
    return false;
}


//==============================================================================
// gfx_showmenu(), gfx_setcursor(), gfx_getdropfile(): what REAPER's window does.

namespace
{
    /** As eel_lice's PopulateMenuFromStr: items separated by '|'; '>' opens
        a submenu (the item is its title), '<' closes it after this item, '#'
        greys an item, '!' checks it; an empty item is a separator. Items are
        numbered from 1 in order, submenu titles excepted. */
    void fillMenu (juce::PopupMenu& menu, juce::StringArray& items, int& at, int& id)
    {
        while (at < items.size())
        {
            auto text = items[at++];
            bool sub = false, grey = false, check = false, last = false;
            while (text.isNotEmpty() && juce::String (">#!<").containsChar (text[0]))
            {
                sub = sub || text[0] == '>';
                grey = grey || text[0] == '#';
                check = check || text[0] == '!';
                last = last || text[0] == '<';
                text = text.substring (1);
            }
            if (sub)
            {
                juce::PopupMenu child;
                fillMenu (child, items, at, id);
                menu.addSubMenu (text, child, ! grey);
            }
            else if (text.isNotEmpty())
                menu.addItem (id++, text, ! grey, check);
            else if (! last)
                menu.addSeparator();
            if (last)
                return;
        }
    }
}

// gfx_showmenu() must return the choice from within @gfx, but a modal menu
// in a plugin freezes the host (REAPER on Linux: its own event loop stops).
// So the first call returns 0 and asks for the menu; the display then waits
// for the choice, puts the script back as it was before that frame (the
// snapshot taken while the user acts on it), and runs the frame again, the
// same click and position, gfx_showmenu() returning the choice this time.

int JsfxView::showMenuHook (void* ctx, const char* text, int x, int y)
{
    auto& view = *static_cast<JsfxView*> (ctx);
    const auto menu = juce::String::fromUTF8 (text);
    if (view.menuResult >= 0 && menu == view.menuText)
        return std::exchange (view.menuResult, -1);   // the frame played again
    if (! view.menuOpen && view.menuRequest.isEmpty())
    {
        view.menuRequest = menu;
        view.menuAt = { x, y };
    }
    return 0;
}

void JsfxView::openMenu()
{
    menuText = std::exchange (menuRequest, {});
    replayBefore = before;
    replayBlocks = beforeBlocks;
    replayStrings = beforeStrings;
    replayMouse = mouse;
    replayCap = buttons | modifiers;

    auto items = juce::StringArray::fromTokens (menuText, "|", "");
    juce::PopupMenu menu;
    int at = 0, id = 1;
    fillMenu (menu, items, at, id);

    // Where the script asks (gfx_x, gfx_y: its frame's pixels).
    const float k = retina ? juce::jmax (1.0f, (float) juce::Component::getApproximateScaleFactorForComponent (this)) : 1.0f;
    const auto p = localPointToGlobal (juce::Point<int> (juce::roundToInt ((float) menuAt.x / k), juce::roundToInt ((float) menuAt.y / k)));
    menuOpen = true;
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea ({ p.x, p.y, 1, 1 }),
                        [safe = juce::Component::SafePointer<JsfxView> (this)] (int chosen)
                        {
                            if (safe != nullptr)
                                safe->menuChosen (chosen);
                        });
}

void JsfxView::menuChosen (int chosen)
{
    menuOpen = false;
    buttons = 0;   // the click that opened it went to the menu
    touched();
    if (chosen <= 0 || sx == nullptr || replayBefore.empty())
        return;    // nothing chosen, or nothing to play again from

    // The script as it was before the frame that asked.
    for (std::size_t i = 0; i < vars.size() && i < replayBefore.size(); ++i)
        if (writable[i])
            *vars[i] = replayBefore[i];
    for (auto& [b, copy] : replayBlocks)
        if (auto* block = memoryBlock (b, false))
            std::copy (copy.begin(), copy.end(), block);
    for (auto& [key, text] : replayStrings)
        jsfx_embed_set_string (sx, key.c_str(), text.data(), (int) text.size());

    // That frame again, with the choice.
    const juce::ScopedValueSetter<bool> busy (inFrame, true);
    menuResult = chosen;
    mouse = replayMouse;
    capOverride = replayCap;
    snapshot();
    drawFrame();
    sendChanges();
    capOverride = -1;
    menuResult = -1;
    menuRequest = {};   // a menu asked again in the replay is the same one
}

void JsfxView::setCursorHook (void* ctx, int id, const char*)
{
    using C = juce::MouseCursor;
    C::StandardCursorType type = C::NormalCursor;
    switch (id)   // Windows' IDC_ values, as scripts use them
    {
        case 32513: type = C::IBeamCursor; break;
        case 32514: type = C::WaitCursor; break;
        case 32515: type = C::CrosshairCursor; break;
        case 32642: type = C::TopLeftCornerResizeCursor; break;
        case 32643: type = C::TopRightCornerResizeCursor; break;
        case 32644: type = C::LeftRightResizeCursor; break;
        case 32645: type = C::UpDownResizeCursor; break;
        case 32646: type = C::UpDownLeftRightResizeCursor; break;
        case 32648: type = C::NoCursor; break;
        case 32649: type = C::PointingHandCursor; break;
        default: break;
    }
    static_cast<JsfxView*> (ctx)->setMouseCursor (type);
}

bool JsfxView::isInterestedInFileDrag (const juce::StringArray&)   { return sx != nullptr; }

void JsfxView::filesDropped (const juce::StringArray& files, int x, int y)
{
    if (sx == nullptr)
        return;
    jsfx_embed_gfx_drop (sx, nullptr);   // gfx_getdropfile() sees these, until it clears them
    for (auto& f : files)
        jsfx_embed_gfx_drop (sx, f.toRawUTF8());
    mouse = { x, y };
    touched();
}

} // namespace fxmenet::plugin
