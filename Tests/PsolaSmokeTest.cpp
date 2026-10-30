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
}

int main()
{
    const bool midVoice = runCase(44100.0, 220.0, -5.0);
    const bool lowVoice = runCase(44100.0, 65.0, 0.0);

    if (!midVoice || !lowVoice)
    {
        std::cerr << "PSOLA smoke test failed: midVoice=" << midVoice
                  << ", lowVoice=" << lowVoice << std::endl;
        return 1;
    }

    std::cout << "PSOLA smoke test passed for 220 Hz shifted and 65 Hz unshifted input."
              << std::endl;
    return 0;
}
