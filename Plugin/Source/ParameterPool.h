/*
  ------------------------------------------------------------------------------
    ParameterPool.h

    The 256 generic parameters FxmeNet shows the DAW (plan.md, decision 3):
    the DAW cannot see the parameters of plugins on the server, and a
    plugin's parameter list cannot change after it is loaded, so automation
    goes through a fixed pool. Each pool parameter is either idle or mapped
    to one parameter of one slot; it then takes that parameter's name and
    value texts.

    Threads: the DAW sets values from any thread, the audio thread included;
    a change only raises the parameter's `dirty` flag (an atomic), which the
    processor's timer collects on the message thread and sends to the server
    in one batch. The mapping itself is changed on the message thread; names
    and texts are read by the host from any thread, under a light lock.

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <JuceHeader.h>

#include <FxmeNet/control/ControlMessages.h>

#include <atomic>
#include <cmath>

namespace fxmenet::plugin
{

constexpr int poolSize = 256;

/** Where a pool parameter points. slotId 0: idle. */
struct Mapping
{
    std::uint32_t slotId = 0;
    std::uint32_t paramIndex = 0;

    bool isMapped() const noexcept   { return slotId != 0; }
    bool operator== (const Mapping& o) const noexcept   { return slotId == o.slotId && paramIndex == o.paramIndex; }
};

class PoolParameter : public juce::AudioParameterFloat
{
public:
    explicit PoolParameter (int poolIndex)
        : juce::AudioParameterFloat (juce::ParameterID { idFor (poolIndex), 1 }, defaultNameFor (poolIndex),
                                     juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f),
          index (poolIndex)
    {
    }

    static juce::String idFor (int poolIndex)          { return "p" + juce::String (poolIndex + 1).paddedLeft ('0', 3); }
    static juce::String defaultNameFor (int poolIndex) { return "Param " + juce::String (poolIndex + 1); }

    int getPoolIndex() const noexcept   { return index; }

    /** Message thread: what this parameter now stands for (`info` null: idle). */
    void describe (const juce::String& slotName, const control::ParamInfo* info)
    {
        const juce::SpinLock::ScopedLockType l (textLock);
        if (info == nullptr)
        {
            label = {};
            unit = {};
            steps.clear();
            return;
        }
        label = slotName + ": " + info->name;
        unit = info->label;
        steps = info->stepTexts;
    }

    /** True once after the DAW (or anyone) changed the value. */
    bool takeDirty() noexcept   { return dirty.exchange (false); }

    //==============================================================================
    juce::String getName (int maximumStringLength) const override
    {
        const juce::SpinLock::ScopedLockType l (textLock);
        const auto& name = label.isNotEmpty() ? label : defaultNameFor (index);
        return maximumStringLength > 0 ? name.substring (0, maximumStringLength) : name;
    }

    juce::String getLabel() const override
    {
        const juce::SpinLock::ScopedLockType l (textLock);
        return unit;
    }

    juce::String getText (float value, int maximumStringLength) const override
    {
        juce::String text;
        {
            const juce::SpinLock::ScopedLockType l (textLock);
            if (steps.size() > 1)
                text = steps[juce::jlimit (0, steps.size() - 1, (int) std::lround (value * (float) (steps.size() - 1)))];
        }
        if (text.isEmpty())
            text = juce::String (value, 3);
        return maximumStringLength > 0 ? text.substring (0, maximumStringLength) : text;
    }

protected:
    void valueChanged (float) override   { dirty.store (true); }   // any thread, the audio thread included

private:
    const int index;
    std::atomic<bool> dirty { false };

    mutable juce::SpinLock textLock;
    juce::String label, unit;
    juce::StringArray steps;   ///< the server's texts for a stepped parameter

    JUCE_DECLARE_NON_COPYABLE (PoolParameter)
};

} // namespace fxmenet::plugin
