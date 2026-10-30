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

int main()
{
    juce::ScopedJuceInitialiser_GUI initialiseJuce;
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
