#include "PluginProcessor.h"
#include "PluginEditor.h"

VantaAudioProcessor::VantaAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", vanta::createLayout())
{
    for (int b = 0; b < vanta::kNumBands; ++b)
        bandPtrs[(size_t) b] = vanta::getBandParamPtrs (apvts, b);

    bypassPtr = apvts.getRawParameterValue (vanta::bypassId());
}

void VantaAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate);
    engine.reset();

    dryBuffer.setSize (2, juce::jmax (64, samplesPerBlock), false, true, false);
    dryBuffer.clear();

    bypassStep = (float) (1.0 / (0.015 * sampleRate));      // 15 ms bypass crossfade
    bypassMix = (bypassPtr->load() > 0.5f) ? 1.0f : 0.0f;
    engineAsleep = (bypassMix >= 1.0f);
}

bool VantaAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return out == layouts.getMainInputChannelSet();
}

void VantaAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numCh = juce::jmin (buffer.getNumChannels(), 2);
    const int numSamples = buffer.getNumSamples();

    for (int c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, numSamples);

    if (numCh <= 0 || numSamples <= 0)
        return;

    const float bypassTarget = (bypassPtr->load() > 0.5f) ? 1.0f : 0.0f;

    // Fully bypassed and settled: zero CPU, audio passes through untouched.
    if (bypassMix >= 1.0f && bypassTarget >= 1.0f)
    {
        engineAsleep = true;
        return;
    }

    if (engineAsleep)
    {
        engine.reset();
        engineAsleep = false;
    }

    for (int b = 0; b < vanta::kNumBands; ++b)
    {
        const auto& p = bandPtrs[(size_t) b];
        hexq::dsp::BandSettings s;
        s.enabled = p.on->load() > 0.5f;
        s.type    = (hexq::dsp::FilterType) juce::jlimit (0, 4, (int) p.type->load());
        s.freq    = p.freq->load();
        s.gainDb  = p.gain->load();
        s.q       = p.q->load();
        engine.setBand (b, s);
    }

    float* ptrs[2] = { buffer.getWritePointer (0), numCh > 1 ? buffer.getWritePointer (1) : nullptr };
    const int maxChunk = juce::jmax (1, dryBuffer.getNumSamples());

    for (int start = 0; start < numSamples; start += maxChunk)
    {
        const int n = juce::jmin (maxChunk, numSamples - start);

        float* chunk[2] = { ptrs[0] + start, ptrs[1] != nullptr ? ptrs[1] + start : nullptr };
        const bool blend = (bypassTarget > 0.0f) || (bypassMix > 0.0f);

        float* dry[2] = { dryBuffer.getWritePointer (0), dryBuffer.getWritePointer (1) };

        if (blend)
            for (int c = 0; c < numCh; ++c)
                juce::FloatVectorOperations::copy (dry[c], chunk[c], n);

        engine.process (chunk, numCh, n);

        if (blend)
        {
            for (int i = 0; i < n; ++i)
            {
                if (bypassMix < bypassTarget)      bypassMix = juce::jmin (bypassTarget, bypassMix + bypassStep);
                else if (bypassMix > bypassTarget) bypassMix = juce::jmax (bypassTarget, bypassMix - bypassStep);

                for (int c = 0; c < numCh; ++c)
                    chunk[c][i] += (dry[c][i] - chunk[c][i]) * bypassMix;
            }
        }
    }
}

juce::AudioProcessorEditor* VantaAudioProcessor::createEditor()
{
    return new VantaAudioProcessorEditor (*this);
}

void VantaAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void VantaAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VantaAudioProcessor();
}
