#include "PluginProcessor.h"
#include <iostream>
#include <vector>
#include <cmath>

static int measurePeak(TunePXOCobiAudioProcessor& processor, int engine, double sampleRate)
{
    auto* parameter = processor.parameters.getParameter("ENGINE");
    parameter->setValueNotifyingHost(engine == 0 ? 0.0f : 1.0f);
    processor.prepareToPlay(sampleRate, 128);

    constexpr int totalSamples = 12000;
    constexpr int blockSize = 128;
    juce::MidiBuffer midi;
    int peakIndex = -1;
    float peakValue = 0.0f;
    int absolute = 0;

    while (absolute < totalSamples)
    {
        juce::AudioBuffer<float> block(2, blockSize);
        block.clear();
        if (absolute == 0)
        {
            block.setSample(0, 0, 1.0f);
            block.setSample(1, 0, 1.0f);
        }
        processor.processBlock(block, midi);
        for (int i = 0; i < blockSize; ++i)
        {
            const float value = std::abs(block.getSample(0, i));
            if (value > peakValue)
            {
                peakValue = value;
                peakIndex = absolute + i;
            }
        }
        absolute += blockSize;
    }

    std::cout << "engine=" << (engine == 0 ? "LEGACY" : "PSOLA")
              << " sampleRate=" << sampleRate
              << " reportedLatency=" << processor.getLatencySamples()
              << " impulsePeak=" << peakIndex
              << " peakValue=" << peakValue << std::endl;
    return peakIndex;
}


static bool testDynamicEngineTransitions()
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 128;
    constexpr int totalBlocks = 360;
    constexpr int firstSwitchBlock = 140;
    constexpr int secondSwitchBlock = 240;
    constexpr float amplitude = 0.2f;
    constexpr float frequency = 220.0f;

    TunePXOCobiAudioProcessor processor;
    processor.prepareToPlay(sampleRate, blockSize);
    auto* engine = processor.parameters.getParameter("ENGINE");
    if (engine == nullptr)
        return false;

    juce::MidiBuffer midi;
    float previous[2] = { 0.0f, 0.0f };
    float maxAdjacentDelta = 0.0f;
    bool allFinite = true;
    int absoluteSample = 0;

    for (int blockIndex = 0; blockIndex < totalBlocks; ++blockIndex)
    {
        if (blockIndex == firstSwitchBlock)
            engine->setValueNotifyingHost(1.0f);
        else if (blockIndex == secondSwitchBlock)
            engine->setValueNotifyingHost(0.0f);

        juce::AudioBuffer<float> block(2, blockSize);
        for (int i = 0; i < blockSize; ++i)
        {
            const float input = amplitude * std::sin(
                juce::MathConstants<float>::twoPi * frequency
                * static_cast<float>(absoluteSample + i) / static_cast<float>(sampleRate));
            block.setSample(0, i, input);
            block.setSample(1, i, input);
        }

        processor.processBlock(block, midi);
        for (int i = 0; i < blockSize; ++i)
        {
            for (int channel = 0; channel < 2; ++channel)
            {
                const float value = block.getSample(channel, i);
                if (!std::isfinite(value))
                    allFinite = false;
                maxAdjacentDelta = juce::jmax(maxAdjacentDelta, std::abs(value - previous[channel]));
                previous[channel] = value;
            }
        }
        absoluteSample += blockSize;
    }

    std::cout << "dynamicEngineTransitions finite=" << allFinite
              << " maxAdjacentDelta=" << maxAdjacentDelta
              << " switches=2" << std::endl;
    return allFinite && maxAdjacentDelta < 1.0f;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    if (!testDynamicEngineTransitions())
        return 2;

    for (double sampleRate : { 44100.0, 48000.0, 96000.0 })
    {
        int legacyPeak = -1;
        int psolaPeak = -1;
        {
            TunePXOCobiAudioProcessor processor;
            legacyPeak = measurePeak(processor, 0, sampleRate);
        }
        {
            TunePXOCobiAudioProcessor processor;
            psolaPeak = measurePeak(processor, 1, sampleRate);
        }
        if (legacyPeak < 0 || psolaPeak < 0)
            return 1;
        std::cout << "enginePeakDifference=" << std::abs(legacyPeak - psolaPeak) << std::endl;
    }
    return 0;
}
