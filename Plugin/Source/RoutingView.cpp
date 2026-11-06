/*
  ------------------------------------------------------------------------------
    RoutingView.cpp

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "RoutingView.h"
#include "Theme.h"

namespace fxmenet::plugin
{

using chain::Pin;
using chain::Connection;

namespace
{
    constexpr float margin = 14.0f;
    constexpr float pinSpacing = 18.0f;
    constexpr float busGap = 14.0f;
    constexpr float boxPad = 12.0f;
    constexpr float slotHeight = 58.0f;
    constexpr float endHeight = 36.0f;
    constexpr float rowGap = 42.0f;
    constexpr float pinRadius = 4.5f;
    constexpr float minBoxWidth = 170.0f;

    juce::String channelName (int c)    { return juce::String (c + 1); }
}

//==============================================================================
class RoutingView::Canvas : public juce::Component,
                            public juce::SettableTooltipClient
{
public:
    explicit Canvas (FxmeNetAudioProcessor& p) : processor (p) {}

    //==============================================================================
    /** Rebuilds the layout when the routing, the chain or the host channels
        changed; true when it did. */
    bool update()
    {
        auto& engine = processor.getEngine();
        auto model = engine.getRouting();
        const auto slots = engine.getSlotInfos();
        const int ins = juce::jmax (1, processor.getPreparedInputs());
        const int outs = juce::jmax (1, processor.getPreparedOutputs());

        // What is drawn, as text: compared with the last one.
        juce::String sig;
        sig << ins << "/" << outs << "|";
        for (auto& b : model.getBoxes())
        {
            sig << (int) b.id << ":" << b.numInputs() << "," << b.numOutputs() << ";";
            for (auto& s : slots)
                if (s.slotId == b.id)
                    sig << s.name << (s.loaded ? "+" : "-") << (s.bypassed ? "b" : "") << ";";
        }
        for (auto& c : model.getConnections())
            sig << (int) c.from.box << "." << c.from.channel << ">" << (int) c.to.box << "." << c.to.channel
                << (model.isManual (c.to) ? "m" : "") << " ";
        for (auto& m : model.getManual())
            sig << "M" << (int) m.input.box << "." << m.input.channel << " ";
        if (sig == signature)
            return false;
        signature = sig;
        routing = std::move (model);
        layOut (slots, ins, outs);
        repaint();
        return true;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::panel);

        for (auto& b : boxes)
            paintBox (g, b);

        for (std::size_t i = 0; i < wires.size(); ++i)
        {
            const auto& w = wires[i];
            const bool hot = (int) i == hoveredWire;
            auto colour = w.manual ? theme::accent : theme::dimText.withAlpha (0.55f);
            if (hot)
                colour = shiftHeld ? theme::error : colour.brighter (0.4f);
            g.setColour (colour);
            g.strokePath (w.path, juce::PathStrokeType (hot ? 2.6f : (w.manual ? 1.8f : 1.3f)));
        }

        for (auto& p : pins)
            paintPin (g, p);

        if (dragging)
        {
            g.setColour (theme::accent.withAlpha (0.8f));
            const juce::Line<float> line (dragFrom, dragPos);
            const float dashes[] = { 5.0f, 4.0f };
            g.drawDashedLine (line, dashes, 2, 1.6f);
            for (auto* p : dropTargets)
            {
                g.setColour (theme::accent);
                g.drawEllipse (juce::Rectangle<float> (p->pos, p->pos).expanded (pinRadius + 3.0f), 1.6f);
            }
        }
    }

    //==============================================================================
    void mouseMove (const juce::MouseEvent& e) override
    {
        shiftHeld = e.mods.isShiftDown();
        const auto* pin = pinAt (e.position);
        const auto* bus = pin == nullptr ? busAt (e.position) : nullptr;
        const int wire = pin == nullptr && bus == nullptr ? wireAt (e.position) : -1;

        if (pin != hoveredPin || wire != hoveredWire)
        {
            hoveredPin = pin;
            hoveredWire = wire;
            repaint();
        }

        if (pin != nullptr)
            setTooltip (describe (*pin));
        else if (bus != nullptr)
            setTooltip (bus->name + ": drag onto a bus or a pin to connect its channels in order.");
        else if (wire >= 0)
            setTooltip (describe (wires[(std::size_t) wire]) + "\nShift-click to remove it.");
        else
            setTooltip ({});
    }

    void modifierKeysChanged (const juce::ModifierKeys& mods) override
    {
        if (shiftHeld != mods.isShiftDown())
        {
            shiftHeld = mods.isShiftDown();
            if (hoveredWire >= 0)
                repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hoveredPin = nullptr;
        hoveredWire = -1;
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto& engine = processor.getEngine();
        dragging = false;
        dragSource.clear();

        if (e.mods.isPopupMenu())
        {
            if (const auto* pin = pinAt (e.position); pin != nullptr && ! pin->output)
                pinMenu (*pin);
            return;
        }

        if (e.mods.isShiftDown())
        {
            const int w = wireAt (e.position);
            if (w >= 0)
            {
                const auto c = wires[(std::size_t) w].connection;
                hoveredWire = -1;
                engine.disconnectPins (c.from, c.to);
                update();
            }
            return;
        }

        if (const auto* pin = pinAt (e.position))
        {
            dragSource = { pin };
            dragFromOutput = pin->output;
            dragFrom = pin->pos;
        }
        else if (const auto* bus = busAt (e.position))
        {
            for (auto& p : pins)
                if (p.box == bus->box && p.output == bus->output && p.bus == bus->bus)
                    dragSource.push_back (&p);
            dragFromOutput = bus->output;
            dragFrom = bus->label.getCentre();
        }
        dragging = ! dragSource.empty();
        dragPos = e.position;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging)
            return;
        dragPos = e.position;
        dropTargets = targetsAt (e.position);
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! dragging)
            return;
        dragging = false;
        const auto targets = targetsAt (e.position);
        dropTargets.clear();

        auto& engine = processor.getEngine();
        for (std::size_t k = 0; k < targets.size() && k < dragSource.size(); ++k)
        {
            const auto* a = dragSource[k];
            const auto* b = targets[k];
            const auto& out = dragFromOutput ? *a : *b;
            const auto& in = dragFromOutput ? *b : *a;
            engine.connectPins ({ out.box, out.channel }, { in.box, in.channel });
        }
        dragSource.clear();
        update();
        repaint();
    }

private:
    struct PinShape
    {
        std::uint32_t box = 0;
        int channel = 0;      ///< across the box's buses (Routing.h)
        bool output = false;
        int bus = 0;
        bool enabled = true;  ///< its bus is enabled (aux buses: when connected)
        bool connected = false;
        juce::Point<float> pos;
        juce::String boxName, busName;
        int indexInBus = 0;
    };

    struct BusShape
    {
        std::uint32_t box = 0;
        bool output = false;
        int bus = 0;
        juce::String name;
        juce::Rectangle<float> label;
        bool enabled = true;
    };

    struct BoxShape
    {
        std::uint32_t id = 0;
        juce::Rectangle<float> rect;
        juce::String name, note;
        bool end = false;
        bool loaded = true;
    };

    struct Wire
    {
        Connection connection;
        bool manual = false;
        juce::Path path;
    };

    //==============================================================================
    void layOut (const std::vector<client::SlotModel>& slots, int ins, int outs)
    {
        boxes.clear();
        pins.clear();
        buses.clear();
        wires.clear();
        hoveredPin = nullptr;
        hoveredWire = -1;
        dropTargets.clear();
        dragSource.clear();
        dragging = false;

        const juce::Font nameFont (juce::FontOptions (13.0f, juce::Font::bold));
        float y = margin, right = 0.0f;
        int index = 0;

        for (auto& box : routing.getBoxes())
        {
            const bool isInputs = box.id == chain::inputsBox, isOutputs = box.id == chain::outputsBox;
            BoxShape shape;
            shape.id = box.id;
            shape.end = isInputs || isOutputs;

            // The buses drawn: the ends at the host's channel counts.
            std::vector<chain::Bus> inBuses = box.inputs, outBuses = box.outputs;
            if (isInputs)
            {
                shape.name = "INPUTS sent to the server";
                outBuses = { { "", ins, true } };
            }
            else if (isOutputs)
            {
                shape.name = "OUTPUTS received from the server";
                inBuses = { { "", outs, true } };
            }
            else
            {
                ++index;
                shape.name = juce::String (index) + ". ";
                for (auto& s : slots)
                    if (s.slotId == box.id)
                    {
                        shape.name << s.name;
                        shape.loaded = s.loaded;
                        if (! s.loaded)
                            shape.note = "not loaded on the server: bypassed";
                        else if (s.bypassed)
                            shape.note = "bypassed";
                    }
            }

            auto groupWidth = [] (const std::vector<chain::Bus>& bs)
            {
                float w = 0.0f;
                int used = 0;
                for (auto& b : bs)
                    if (b.channels > 0)
                    {
                        w += (float) b.channels * pinSpacing;
                        ++used;
                    }
                return w + (float) juce::jmax (0, used - 1) * busGap;
            };
            const float width = juce::jmax (minBoxWidth,
                                            juce::GlyphArrangement::getStringWidth (nameFont, shape.name) + 2.0f * boxPad,
                                            groupWidth (inBuses) + 2.0f * boxPad,
                                            groupWidth (outBuses) + 2.0f * boxPad);
            const float height = shape.end ? endHeight : slotHeight;
            shape.rect = { margin, y, width, height };

            auto place = [&] (const std::vector<chain::Bus>& bs, bool output)
            {
                float x = shape.rect.getX() + boxPad;
                int channel = 0;
                for (int b = 0; b < (int) bs.size(); ++b)
                {
                    const auto& bus = bs[(std::size_t) b];
                    if (bus.channels <= 0)
                        continue;
                    BusShape busShape;
                    busShape.box = box.id;
                    busShape.output = output;
                    busShape.bus = b;
                    busShape.name = bus.name.empty() ? juce::String (output ? "Outputs" : "Inputs") : juce::String (bus.name);
                    busShape.enabled = shape.end || bus.main || routing.isBusEnabled (box.id, ! output, b);
                    const float w = (float) bus.channels * pinSpacing;
                    const float labelY = output ? shape.rect.getBottom() - 19.0f : shape.rect.getY() + 7.0f;
                    busShape.label = { x, labelY, w, 12.0f };
                    buses.push_back (busShape);

                    for (int k = 0; k < bus.channels; ++k, ++channel)
                    {
                        PinShape p;
                        p.box = box.id;
                        p.channel = channel;
                        p.output = output;
                        p.bus = b;
                        p.indexInBus = k;
                        p.enabled = busShape.enabled;
                        p.pos = { x + ((float) k + 0.5f) * pinSpacing, output ? shape.rect.getBottom() : shape.rect.getY() };
                        p.boxName = shape.name;
                        p.busName = bus.name;
                        pins.push_back (p);
                    }
                    x += w + busGap;
                }
            };
            place (inBuses, false);
            place (outBuses, true);

            boxes.push_back (shape);
            right = juce::jmax (right, shape.rect.getRight());
            y += height + rowGap;
        }

        // The connections between pins shown (channels past the host's are not).
        for (auto& c : routing.getConnections())
        {
            const auto* a = find (c.from, true);
            const auto* b = find (c.to, false);
            if (a == nullptr || b == nullptr)
                continue;
            Wire w;
            w.connection = c;
            w.manual = routing.isManual (c.to);
            const float bend = juce::jmax (18.0f, (b->pos.y - a->pos.y) * 0.45f);
            w.path.startNewSubPath (a->pos);
            w.path.cubicTo (a->pos.translated (0.0f, bend), b->pos.translated (0.0f, -bend), b->pos);
            wires.push_back (std::move (w));
        }
        for (auto& p : pins)
            for (auto& w : wires)
                if ((p.output && w.connection.from == Pin { p.box, p.channel })
                    || (! p.output && w.connection.to == Pin { p.box, p.channel }))
                    p.connected = true;

        setSize ((int) (right + margin), (int) (y - rowGap + margin));
    }

    //==============================================================================
    void paintBox (juce::Graphics& g, const BoxShape& b)
    {
        g.setColour (b.end ? theme::topBarBg : theme::background);
        g.fillRoundedRectangle (b.rect, 5.0f);
        g.setColour (b.loaded ? theme::panelLine : theme::warning.withAlpha (0.6f));
        g.drawRoundedRectangle (b.rect, 5.0f, 1.0f);

        auto text = b.rect.reduced (boxPad, 0.0f);
        if (! b.end)
            text = text.withTrimmedTop (18.0f).withTrimmedBottom (18.0f);
        g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        g.setColour (b.end ? theme::dimText : theme::text);
        if (b.note.isEmpty())
            g.drawText (b.name, text, juce::Justification::centredLeft, true);
        else
        {
            g.drawText (b.name, text.removeFromLeft (text.getWidth() * 0.6f), juce::Justification::centredLeft, true);
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.setColour (b.loaded ? theme::dimText : theme::warning);
            g.drawText (b.note, text, juce::Justification::centredRight, true);
        }

        // Bus names (not for the ends: one unnamed bus each).
        if (b.end)
            return;
        g.setFont (juce::Font (juce::FontOptions (10.5f)));
        for (auto& bus : buses)
            if (bus.box == b.id)
            {
                g.setColour (bus.enabled ? theme::dimText : theme::dimText.withAlpha (0.45f));
                g.drawText (bus.name, bus.label.withWidth (juce::jmax (bus.label.getWidth(), 60.0f)),
                            juce::Justification::centredLeft, true);
            }
    }

    void paintPin (juce::Graphics& g, const PinShape& p)
    {
        const bool hot = &p == hoveredPin;
        const float r = hot ? pinRadius + 1.5f : pinRadius;
        const auto area = juce::Rectangle<float> (p.pos, p.pos).expanded (r);
        if (! p.enabled)
        {
            g.setColour (theme::dimText.withAlpha (0.6f));
            g.drawEllipse (area, 1.2f);
            return;
        }
        g.setColour (p.connected ? theme::accent : theme::panelLine.brighter (0.3f));
        g.fillEllipse (area);
        if (hot)
        {
            g.setColour (theme::text);
            g.drawEllipse (area, 1.2f);
        }
    }

    //==============================================================================
    const PinShape* find (Pin pin, bool output) const
    {
        for (auto& p : pins)
            if (p.output == output && p.box == pin.box && p.channel == pin.channel)
                return &p;
        return nullptr;
    }

    const PinShape* pinAt (juce::Point<float> at) const
    {
        const PinShape* best = nullptr;
        float bestDistance = pinRadius + 5.0f;
        for (auto& p : pins)
        {
            const float d = p.pos.getDistanceFrom (at);
            if (d < bestDistance)
            {
                best = &p;
                bestDistance = d;
            }
        }
        return best;
    }

    const BusShape* busAt (juce::Point<float> at) const
    {
        for (auto& b : buses)
            if (! isEnd (b.box) && b.label.expanded (2.0f).contains (at))
                return &b;
        return nullptr;
    }

    int wireAt (juce::Point<float> at) const
    {
        int best = -1;
        float bestDistance = 5.0f;
        for (std::size_t i = 0; i < wires.size(); ++i)
        {
            juce::Point<float> nearest;
            wires[i].path.getNearestPoint (at, nearest);
            const float d = nearest.getDistanceFrom (at);
            if (d < bestDistance)
            {
                best = (int) i;
                bestDistance = d;
            }
        }
        return best;
    }

    static bool isEnd (std::uint32_t box)   { return box == chain::inputsBox || box == chain::outputsBox; }

    /** Where a drag would land: the pins on the other side, in order (a
        bus from its first channel, a pin from that one to the end of its
        bus; one pin for a one-pin drag), valid connections only. */
    std::vector<const PinShape*> targetsAt (juce::Point<float> at) const
    {
        std::vector<const PinShape*> run;
        if (const auto* pin = pinAt (at))
        {
            if (pin->output == dragFromOutput)
                return {};
            for (auto& p : pins)
                if (p.box == pin->box && p.output == pin->output && p.bus == pin->bus && p.indexInBus >= pin->indexInBus)
                    run.push_back (&p);
        }
        else if (const auto* bus = busAt (at))
        {
            if (bus->output == dragFromOutput)
                return {};
            for (auto& p : pins)
                if (p.box == bus->box && p.output == bus->output && p.bus == bus->bus)
                    run.push_back (&p);
        }

        std::vector<const PinShape*> valid;
        for (std::size_t k = 0; k < run.size() && k < dragSource.size(); ++k)
        {
            const auto* a = dragSource[k];
            const auto* b = run[k];
            const auto& out = dragFromOutput ? *a : *b;
            const auto& in = dragFromOutput ? *b : *a;
            if (! routing.isValid ({ out.box, out.channel }, { in.box, in.channel }))
                break;
            valid.push_back (b);
        }
        return valid;
    }

    void pinMenu (const PinShape& pin)
    {
        const Pin p { pin.box, pin.channel };
        const bool manual = routing.isManual (p);
        const bool fed = ! routing.sourcesOf (p).empty();

        juce::PopupMenu menu;
        menu.addSectionHeader (describe (pin));
        menu.addItem (1, "Back to the default routing", manual);
        menu.addItem (2, "Disconnect", fed);
        juce::Component::SafePointer<Canvas> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options(), [safe, p] (int result)
        {
            if (safe == nullptr || result == 0)
                return;
            auto& engine = safe->processor.getEngine();
            if (result == 1)
                engine.setPinAutomatic (p);
            else if (result == 2)
                engine.clearPin (p);
            safe->update();
        });
    }

    juce::String describe (const PinShape& p) const
    {
        juce::String s = p.boxName;
        if (! isEnd (p.box))
            s << ", " << (p.output ? "output " : "input ") << (p.busName.isEmpty() ? juce::String() : p.busName + " ")
              << channelName (p.indexInBus);
        else
            s << ", channel " << channelName (p.channel);
        if (! p.enabled)
            s << " (bus disabled until connected)";
        if (! p.output)
            s << (routing.isManual ({ p.box, p.channel }) ? "\nSet by hand: right-click for the default"
                                                          : "\nDefault routing");
        return s;
    }

    juce::String describe (const Wire& w) const
    {
        const auto* a = find (w.connection.from, true);
        const auto* b = find (w.connection.to, false);
        if (a == nullptr || b == nullptr)
            return {};
        auto end = [this] (const PinShape& p)
        {
            return isEnd (p.box) ? p.boxName.upToFirstOccurrenceOf (" ", false, false) + " " + channelName (p.channel)
                                 : p.boxName + " " + (p.busName.isEmpty() ? juce::String() : p.busName + " ")
                                       + channelName (p.indexInBus);
        };
        return end (*a) + "  >  " + end (*b) + (w.manual ? "" : "  (default routing)");
    }

    FxmeNetAudioProcessor& processor;
    chain::Routing routing;
    juce::String signature;

    std::vector<BoxShape> boxes;
    std::vector<BusShape> buses;
    std::vector<PinShape> pins;
    std::vector<Wire> wires;

    const PinShape* hoveredPin = nullptr;
    int hoveredWire = -1;
    bool shiftHeld = false;

    bool dragging = false, dragFromOutput = false;
    std::vector<const PinShape*> dragSource, dropTargets;
    juce::Point<float> dragFrom, dragPos;
};

//==============================================================================
RoutingView::RoutingView (FxmeNetAudioProcessor& p) : canvas (std::make_unique<Canvas> (p))
{
    viewport.setViewedComponent (canvas.get(), false);
    viewport.setScrollBarsShown (true, true, false, false);
    viewport.setScrollBarThickness (8);
    addAndMakeVisible (viewport);
}

RoutingView::~RoutingView() = default;

void RoutingView::refresh()
{
    // Not while the mouse is down on it: a drag in progress keeps its shapes.
    if (! canvas->isMouseButtonDown())
        canvas->update();
}

void RoutingView::resized()
{
    viewport.setBounds (getLocalBounds());
}

} // namespace fxmenet::plugin
