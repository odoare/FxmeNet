/*
  ------------------------------------------------------------------------------
    Shadow.h

    A slot's local copy (Phase 4, step 8b; plan.md section 1): the same
    plugin as on the server, loaded on the client only to show its own
    editor. It never processes audio. FxmeNet-aware plugins are told so
    (INetPlugin role "mirror") before they are prepared, so they can skip
    heavy preparation; their meters are fed from the server in step 8c.

    The shadow's parameters mirror the server's, both ways:
      - its editor moving a parameter (and its gestures) is reported through
        onParameterChanged / onGesture, on the message thread;
      - values from elsewhere (the server, the DAW's automation, the generic
        view) are set with setQuietly(), which reaches the editor without
        telling anyone. A parameter the editor touched in the last
        `touchHoldMs` is left alone, so that a value on its way to the server
        does not snap the control back.

    Message thread only, apart from the parameter listener (which may be
    called on another thread, and then posts to the message thread).

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>

#include <FxmeNet/catalog/Buses.h>
#include <FxmeTools/net/NetAbi.h>

#include <functional>
#include <memory>
#include <vector>

namespace fxmenet::plugin
{

class Shadow : private juce::AudioProcessorParameter::Listener
{
public:
    static constexpr juce::uint32 touchHoldMs = 300;

    /** Loads `description` as a mirror; nullptr and `error` on failure. */
    static std::unique_ptr<Shadow> create (juce::AudioPluginFormatManager& formats,
                                           const juce::PluginDescription& description,
                                           double sampleRate, int blockSize, juce::String& error)
    {
        auto instance = formats.createPluginInstance (description, sampleRate, blockSize, error);
        if (instance == nullptr)
            return nullptr;
        return std::unique_ptr<Shadow> (new Shadow (std::move (instance), sampleRate, blockSize));
    }

    ~Shadow() override
    {
        jassert (instance->getActiveEditor() == nullptr);   // the view goes first
        for (auto* p : instance->getParameters())
            p->removeListener (this);
        if (net != nullptr)
            net->release();
        instance->releaseResources();
    }

    juce::AudioPluginInstance& getInstance() noexcept       { return *instance; }

    /** The side channel, or null (a plugin that is not FxmeNet-aware). */
    fxme::net::INetPlugin* getNet() const noexcept          { return net; }

    int getNumParameters() const                            { return instance->getParameters().size(); }

    /** A value from elsewhere: shown, nothing reported. False when the
        editor touched the parameter too recently. */
    bool setQuietly (int index, float value)
    {
        auto& params = instance->getParameters();
        if (! juce::isPositiveAndBelow (index, params.size()))
            return false;
        if (juce::Time::getMillisecondCounter() - touched[(std::size_t) index] < touchHoldMs)
            return false;
        auto* p = params[index];
        if (std::abs (p->getValue() - value) > 1.0e-6f)
            p->setValue (value);   // setValue: the listeners are not called
        return true;
    }

    float getValue (int index) const
    {
        auto& params = instance->getParameters();
        return juce::isPositiveAndBelow (index, params.size()) ? params[index]->getValue() : 0.0f;
    }

    /** Message thread: the editor moved parameter `index` / started or
        ended a gesture on it. */
    std::function<void (int index, float value)> onParameterChanged;
    std::function<void (int index, bool starting)> onGesture;

    /** The state-change counter of an FxmeNet-aware plugin (0 otherwise). */
    std::uint32_t getStateChangeCount() const               { return net != nullptr ? net->getStateChangeCount() : 0; }

    std::uint32_t lastStateChangeCount = 0;   ///< what the processor last acted on
    bool stateReceived = false;               ///< the server's state was applied

private:
    Shadow (std::unique_ptr<juce::AudioPluginInstance> loaded, double sampleRate, int blockSize)
        : instance (std::move (loaded))
    {
        if (auto* vst3 = instance->getVST3Client())
            net = fxme::net::queryNetPlugin (vst3->getIComponentPtr());
        if (net != nullptr)
            net->setRole ((std::int32_t) fxme::net::Role::mirror);   // before preparing

        catalog::useDefaultBuses (*instance);   // as on the server (Buses.h)
        instance->prepareToPlay (sampleRate, blockSize);   // editors read the rate; never processed
        lastStateChangeCount = getStateChangeCount();

        touched.assign ((std::size_t) instance->getParameters().size(), 0);
        for (auto* p : instance->getParameters())
            p->addListener (this);
    }

    void parameterValueChanged (int index, float value) override
    {
        post ([index, value] (Shadow& s)
        {
            if (juce::isPositiveAndBelow (index, (int) s.touched.size()))
                s.touched[(std::size_t) index] = juce::Time::getMillisecondCounter();
            if (s.onParameterChanged)
                s.onParameterChanged (index, value);
        });
    }

    void parameterGestureChanged (int index, bool starting) override
    {
        post ([index, starting] (Shadow& s)
        {
            if (s.onGesture)
                s.onGesture (index, starting);
        });
    }

    /** Runs `f` on the message thread: now if already there. */
    template <typename Function>
    void post (Function f)
    {
        if (juce::MessageManager::existsAndIsCurrentThread())
        {
            f (*this);
            return;
        }
        juce::WeakReference<Shadow> weak (this);
        juce::MessageManager::callAsync ([weak, f]
        {
            if (auto* s = weak.get())
                f (*s);
        });
    }

    std::unique_ptr<juce::AudioPluginInstance> instance;
    fxme::net::INetPlugin* net = nullptr;
    std::vector<juce::uint32> touched;   ///< when the editor last moved each parameter

    JUCE_DECLARE_WEAK_REFERENCEABLE (Shadow)
    JUCE_DECLARE_NON_COPYABLE (Shadow)
};

} // namespace fxmenet::plugin
