#include "sola.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace
{
double estimateFrequency(const std::vector<float>& samples, double sampleRate,
                         double expectedFrequency)
{
    // Direct spectral scan around the expected fundamental. This avoids
    // autocorrelation selecting a longer sub-period in the PSOLA waveform.
    const size_t count = std::min<size_t>(samples.size(), 32768);
    if (count < 4096)
        return 0.0;

    const double minFrequency = expectedFrequency * 0.70;
    const double maxFrequency = expectedFrequency * 1.30;
    constexpr double frequencyStep = 0.25;
    double bestFrequency = 0.0;
    double bestMagnitude = -1.0;

    for (double frequency = minFrequency; frequency <= maxFrequency;
         frequency += frequencyStep)
    {
        double real = 0.0;
        double imag = 0.0;
        for (size_t i = 0; i < count; ++i)
        {
            const double window = 0.5 - 0.5 * std::cos(
                juce::MathConstants<double>::twoPi * static_cast<double>(i)
                / static_cast<double>(count - 1));
            const double angle = juce::MathConstants<double>::twoPi
                * frequency * static_cast<double>(i) / sampleRate;
            const double value = static_cast<double>(samples[i]) * window;
            real += value * std::cos(angle);
            imag -= value * std::sin(angle);
        }
        const double magnitude = real * real + imag * imag;
        if (magnitude > bestMagnitude)
        {
            bestMagnitude = magnitude;
            bestFrequency = frequency;
        }
    }
    return bestFrequency;
}

bool runCase(double sampleRate, double frequency, double semitones)
{
    constexpr int channels = 1;
    // The upstream PSOLA implementation is designed for analysis frames
    // around 50 ms; 256-sample host blocks are too short for its mark scheduler.
    constexpr int frameSize = 2048;
    const int maxPeriod = static_cast<int>(std::ceil(sampleRate / 65.0));
    const int period = static_cast<int>(std::lround(sampleRate / frequency));
    const double expectedOutput = frequency * std::pow(2.0, semitones / 12.0);

    TD_pitch_shift::Sola processor(channels, frameSize, maxPeriod);
    juce::AudioBuffer<float> frame(channels, frameSize);
    std::vector<float> captured;
    captured.reserve(400 * frameSize);

    double phase = 0.0;
    const double phaseStep = juce::MathConstants<double>::twoPi * frequency / sampleRate;

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
            if (block >= 80)
                captured.push_back(samples[i]);
        }
    }

    double energy = 0.0;
    for (float sample : captured)
        energy += static_cast<double>(sample) * sample;
    const double measuredFrequency = estimateFrequency(captured, sampleRate, expectedOutput);
    const double errorCents = measuredFrequency > 0.0
        ? 1200.0 * std::log2(measuredFrequency / expectedOutput) : 10000.0;

    std::cout << "PSOLA input=" << frequency << " Hz, shift=" << semitones
              << " st, expected=" << expectedOutput << " Hz, measured="
              << measuredFrequency << " Hz, error=" << errorCents << " cents"
              << std::endl;

    return !captured.empty() && energy / captured.size() > 1.0e-6
        && std::abs(errorCents) <= 35.0;
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
    const bool shiftedVoice = runCase(44100.0, 220.0, -5.0);
    const bool unshiftedLowVoice = runCase(44100.0, 65.0, 0.0);
    const int impulsePeak = measureImpulsePeak();

    if (!shiftedVoice || !unshiftedLowVoice || impulsePeak < 0)
    {
        std::cerr << "PSOLA quality test failed: shiftedVoice=" << shiftedVoice
                  << ", unshiftedLowVoice=" << unshiftedLowVoice
                  << ", impulsePeak=" << impulsePeak << std::endl;
        return 1;
    }

    std::cout << "PSOLA frequency and impulse tests passed." << std::endl;
    return 0;
}
