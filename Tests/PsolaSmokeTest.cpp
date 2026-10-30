#include "sola.h"

#include <cmath>
#include <iostream>

namespace
{
bool runCase(double sampleRate, double frequency, double semitones)
{
    constexpr int channels = 1;
    constexpr int frameSize = 256;
    const int maxPeriod = static_cast<int>(std::ceil(sampleRate / 65.0));
    const int period = static_cast<int>(std::lround(sampleRate / frequency));

    TD_pitch_shift::Sola processor(channels, frameSize, maxPeriod);
    juce::AudioBuffer<float> frame(channels, frameSize);

    double phase = 0.0;
    const double phaseStep = juce::MathConstants<double>::twoPi * frequency / sampleRate;
    double outputEnergy = 0.0;
    int outputSamples = 0;

    for (int block = 0; block < 400; ++block)
    {
        auto* samples = frame.getWritePointer(0);
        for (int i = 0; i < frameSize; ++i)
        {
            samples[i] = 0.5f * static_cast<float>(std::sin(phase));
            phase += phaseStep;
            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;
        }

        processor.Process(frame, period, semitones);

        for (int i = 0; i < frameSize; ++i)
        {
            if (!std::isfinite(samples[i]))
                return false;
            if (block > 40)
            {
                outputEnergy += static_cast<double>(samples[i]) * samples[i];
                ++outputSamples;
            }
        }
    }

    return outputSamples > 0 && outputEnergy / outputSamples > 1.0e-6;
}

int measureImpulsePeak()
{
    constexpr int channels = 1;
    constexpr int frameSize = 256;
    constexpr int maxPeriod = 679;
    constexpr int totalFrames = 300;
    TD_pitch_shift::Sola processor(channels, frameSize, maxPeriod);
    juce::AudioBuffer<float> frame(channels, frameSize);
    int sampleIndex = 0;
    int peakIndex = -1;
    float peak = 0.0f;

    for (int block = 0; block < totalFrames; ++block)
    {
        frame.clear();
        if (block == 0)
            frame.setSample(0, 0, 1.0f);

        processor.Process(frame, 200, -5.0);

        for (int i = 0; i < frameSize; ++i, ++sampleIndex)
        {
            const float value = std::abs(frame.getSample(0, i));
            if (value > peak)
            {
                peak = value;
                peakIndex = sampleIndex;
            }
        }
    }

    std::cout << "PSOLA impulse peak index=" << peakIndex
              << ", peak=" << peak << std::endl;
    return peakIndex;
}
}

int main()
{
    const bool midVoice = runCase(44100.0, 220.0, -5.0);
    const bool lowVoice = runCase(44100.0, 65.0, 0.0);
    const int impulsePeak = measureImpulsePeak();

    if (!midVoice || !lowVoice || impulsePeak < 0)
    {
        std::cerr << "PSOLA smoke test failed: midVoice=" << midVoice
                  << ", lowVoice=" << lowVoice
                  << ", impulsePeak=" << impulsePeak << std::endl;
        return 1;
    }

    std::cout << "PSOLA smoke test passed; impulse peak sample="
              << impulsePeak << std::endl;
    return 0;
}
