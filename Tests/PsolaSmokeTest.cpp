#include "sola.h"

#include <algorithm>
#include <array>
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

bool runCase(double sampleRate, double frequency, double semitones, int frameSize)
{
    constexpr int channels = 1;
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

    std::cout << "PSOLA frame=" << frameSize << ", input=" << frequency << " Hz, shift=" << semitones
              << " st, expected=" << expectedOutput << " Hz, measured="
              << measuredFrequency << " Hz, error=" << errorCents << " cents"
              << std::endl;

    return !captured.empty() && energy / captured.size() > 1.0e-6
        && std::abs(errorCents) <= 35.0;
}

// Test-only fixed-frame adapter: host blocks are accumulated into 1024-sample
// PSOLA frames and output is drained at the host's requested block sizes.
// All storage is allocated up front; processHostBlock performs no allocations.
class VariableBlockAdapter
{
public:
    static constexpr int frameSize = 1024;
    static constexpr int ringSize = 8192;

    VariableBlockAdapter(double sampleRate, double frequency, double semitones)
        : processor(1, frameSize, static_cast<int>(std::ceil(sampleRate / 65.0))),
          frame(1, frameSize),
          period(static_cast<int>(std::lround(sampleRate / frequency))),
          shift(semitones)
    {
        // Prime output with silence to model the adapter's initial buffering.
        for (int i = 0; i < frameSize; ++i)
            pushOutput(0.0f);
    }

    void processHostBlock(const float* input, float* output, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            pushInput(input[i]);
            output[i] = popOutput();
            if (inputCount >= frameSize)
                processFrame();
        }
    }

    int samplesProcessed() const noexcept { return processed; }
    int outputUnderflows() const noexcept { return underflows; }

private:
    void pushInput(float value)
    {
        inputRing[writeInput] = value;
        writeInput = (writeInput + 1) % ringSize;
        ++inputCount;
    }

    void processFrame()
    {
        auto* dst = frame.getWritePointer(0);
        for (int i = 0; i < frameSize; ++i)
        {
            dst[i] = inputRing[readInput];
            readInput = (readInput + 1) % ringSize;
            --inputCount;
        }
        processor.Process(frame, period, shift);
        for (int i = 0; i < frameSize; ++i)
            pushOutput(dst[i]);
        processed += frameSize;
    }

    void pushOutput(float value)
    {
        if (outputCount >= ringSize)
            return;
        outputRing[writeOutput] = value;
        writeOutput = (writeOutput + 1) % ringSize;
        ++outputCount;
    }

    float popOutput()
    {
        if (outputCount == 0)
        {
            ++underflows;
            return 0.0f;
        }
        const float value = outputRing[readOutput];
        readOutput = (readOutput + 1) % ringSize;
        --outputCount;
        return value;
    }

    TD_pitch_shift::Sola processor;
    juce::AudioBuffer<float> frame;
    const int period;
    const double shift;
    std::array<float, ringSize> inputRing{}, outputRing{};
    int readInput=0, writeInput=0, inputCount=0;
    int readOutput=0, writeOutput=0, outputCount=0;
    int processed=0, underflows=0;
};

bool testVariableHostBlocks()
{
    constexpr double sampleRate = 44100.0;
    constexpr double frequency = 220.0;
    constexpr int blockSizes[] = {64, 128, 256, 512, 1024, 192, 768, 48, 320};
    VariableBlockAdapter adapter(sampleRate, frequency, -5.0);
    std::vector<float> input(1024), output(1024);
    double phase = 0.0;
    const double step = juce::MathConstants<double>::twoPi * frequency / sampleRate;
    int total = 0;
    size_t block = 0;

    while (total < 65536)
    {
        const int count = std::min(blockSizes[block++ % (sizeof(blockSizes) / sizeof(blockSizes[0]))], 65536 - total);
        for (int i = 0; i < count; ++i)
        {
            input[static_cast<size_t>(i)] = 0.5f * static_cast<float>(std::sin(phase));
            phase += step;
            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;
        }
        adapter.processHostBlock(input.data(), output.data(), count);
        for (int i = 0; i < count; ++i)
            if (!std::isfinite(output[static_cast<size_t>(i)]))
                return false;
        total += count;
    }

    const bool passed = adapter.samplesProcessed() == 65536
        && adapter.outputUnderflows() == 0;
    std::cout << "Variable host blocks: input=" << total
              << ", PSOLA frames=" << adapter.samplesProcessed()
              << ", output underflows=" << adapter.outputUnderflows()
              << std::endl;
    return passed;
}

int measureImpulsePeak()
{
    constexpr int channels = 1;
    // Match the candidate real-time adapter frame size. This measurement
    // is only an impulse-peak observation, not a host-reported latency value.
    constexpr int frameSize = 1024;
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
    const bool shiftedVoice1024 = runCase(44100.0, 220.0, -5.0, 1024);
    const bool shiftedVoice2048 = runCase(44100.0, 220.0, -5.0, 2048);
    const bool shiftedVoice4096 = runCase(44100.0, 220.0, -5.0, 4096);
    const bool unshiftedLowVoice = runCase(44100.0, 65.0, 0.0, 2048);
    const int impulsePeak = measureImpulsePeak();
    const bool variableBlocks = testVariableHostBlocks();

    if (!shiftedVoice1024 || !shiftedVoice2048 || !shiftedVoice4096 || !unshiftedLowVoice || impulsePeak < 0 || !variableBlocks)
    {
        std::cerr << "PSOLA quality test failed: shifted2048=" << shiftedVoice2048
                  << ", shifted1024=" << shiftedVoice1024 << ", shifted2048=" << shiftedVoice2048
                  << ", shifted4096=" << shiftedVoice4096 << ", unshiftedLowVoice=" << unshiftedLowVoice
                  << ", impulsePeak=" << impulsePeak << ", variableBlocks=" << variableBlocks << std::endl;
        return 1;
    }

    std::cout << "PSOLA frequency and impulse tests passed." << std::endl;
    return 0;
}
