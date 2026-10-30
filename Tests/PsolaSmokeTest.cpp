#include "sola.h"
#include "PsolaRealtimeAdapter.h"

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

    VariableBlockAdapter(double sampleRate, double frequency, double semitones, bool bypass = false)
        : processor(1, frameSize, static_cast<int>(std::ceil(sampleRate / 65.0))),
          frame(1, frameSize),
          period(static_cast<int>(std::lround(sampleRate / frequency))),
          shift(semitones),
          bypassProcessing(bypass)
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
    int outputOverflows() const noexcept { return overflows; }

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
        if (!bypassProcessing)
            processor.Process(frame, period, shift);
        for (int i = 0; i < frameSize; ++i)
            pushOutput(dst[i]);
        processed += frameSize;
    }

    void pushOutput(float value)
    {
        if (outputCount >= ringSize)
        {
            ++overflows;
            return;
        }
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
    const bool bypassProcessing;
    std::array<float, ringSize> inputRing{}, outputRing{};
    int readInput=0, writeInput=0, inputCount=0;
    int readOutput=0, writeOutput=0, outputCount=0;
    int processed=0, underflows=0, overflows=0;
};

bool testVariableHostBlocks()
{
    constexpr double sampleRate = 44100.0;
    constexpr double frequency = 220.0;
    constexpr int totalSamples = 65536;
    constexpr int blockSizes[] = {64, 128, 256, 512, 1024, 192, 768, 48, 320};

    VariableBlockAdapter psolaAdapter(sampleRate, frequency, -5.0);
    std::vector<float> input(1024), output(1024);
    double phase = 0.0;
    const double step = juce::MathConstants<double>::twoPi * frequency / sampleRate;
    int total = 0;
    size_t block = 0;

    while (total < totalSamples)
    {
        const int count = std::min(blockSizes[block++ % (sizeof(blockSizes) / sizeof(blockSizes[0]))],
                                   totalSamples - total);
        for (int i = 0; i < count; ++i)
        {
            input[static_cast<size_t>(i)] = 0.5f * static_cast<float>(std::sin(phase));
            phase += step;
            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;
        }
        psolaAdapter.processHostBlock(input.data(), output.data(), count);
        for (int i = 0; i < count; ++i)
            if (!std::isfinite(output[static_cast<size_t>(i)]))
                return false;
        total += count;
    }

    const bool psolaPassed = psolaAdapter.samplesProcessed() == totalSamples
        && psolaAdapter.outputUnderflows() == 0
        && psolaAdapter.outputOverflows() == 0;

    VariableBlockAdapter identityAdapter(sampleRate, frequency, 0.0, true);
    std::vector<float> identityInput(totalSamples), identityOutput(totalSamples);
    for (int i = 0; i < totalSamples; ++i)
        identityInput[static_cast<size_t>(i)] =
            static_cast<float>((i * 37) % 1009) / 1009.0f - 0.5f;

    total = 0;
    block = 0;
    while (total < totalSamples)
    {
        const int count = std::min(blockSizes[block++ % (sizeof(blockSizes) / sizeof(blockSizes[0]))],
                                   totalSamples - total);
        identityAdapter.processHostBlock(identityInput.data() + total,
                                          identityOutput.data() + total, count);
        total += count;
    }

    bool identityPreserved = true;
    for (int i = 0; i < totalSamples; ++i)
    {
        const float expected = i < VariableBlockAdapter::frameSize
            ? 0.0f : identityInput[static_cast<size_t>(i - VariableBlockAdapter::frameSize)];
        if (identityOutput[static_cast<size_t>(i)] != expected)
        {
            identityPreserved = false;
            break;
        }
    }

    const bool identityPassed = identityAdapter.samplesProcessed() == totalSamples
        && identityAdapter.outputUnderflows() == 0
        && identityAdapter.outputOverflows() == 0
        && identityPreserved;

    std::cout << "Variable host blocks: input=" << total
              << ", PSOLA frames=" << psolaAdapter.samplesProcessed()
              << ", PSOLA underflows=" << psolaAdapter.outputUnderflows()
              << ", PSOLA overflows=" << psolaAdapter.outputOverflows()
              << ", identity FIFO=" << (identityPassed ? "exact" : "FAILED")
              << ", identity underflows=" << identityAdapter.outputUnderflows()
              << ", identity overflows=" << identityAdapter.outputOverflows()
              << std::endl;
    return psolaPassed && identityPassed;
}

bool testStereoPsola()
{
    constexpr double sampleRate = 44100.0;
    constexpr double frequency = 220.0;
    constexpr int frameSize = 1024;
    constexpr int channels = 2;
    const int period = static_cast<int>(std::lround(sampleRate / frequency));
    const int maxPeriod = static_cast<int>(std::ceil(sampleRate / 65.0));

    TD_pitch_shift::Sola processor(channels, frameSize, maxPeriod);
    juce::AudioBuffer<float> frame(channels, frameSize);
    std::vector<float> leftCaptured, rightCaptured;
    leftCaptured.reserve(180 * frameSize);
    rightCaptured.reserve(180 * frameSize);

    double phase = 0.0;
    const double step = juce::MathConstants<double>::twoPi * frequency / sampleRate;
    for (int block = 0; block < 220; ++block)
    {
        auto* left = frame.getWritePointer(0);
        auto* right = frame.getWritePointer(1);
        for (int i = 0; i < frameSize; ++i)
        {
            const float sample = 0.4f * static_cast<float>(std::sin(phase));
            left[i] = sample;
            right[i] = 0.25f * static_cast<float>(std::sin(phase + 0.37));
            phase += step;
            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;
        }

        processor.Process(frame, period, -5.0);
        for (int i = 0; i < frameSize; ++i)
        {
            if (!std::isfinite(left[i]) || !std::isfinite(right[i]))
                return false;
            if (block >= 40)
            {
                leftCaptured.push_back(left[i]);
                rightCaptured.push_back(right[i]);
            }
        }
    }

    const double expected = frequency * std::pow(2.0, -5.0 / 12.0);
    const double leftMeasured = estimateFrequency(leftCaptured, sampleRate, expected);
    const double rightMeasured = estimateFrequency(rightCaptured, sampleRate, expected);
    const double leftError = 1200.0 * std::log2(leftMeasured / expected);
    const double rightError = 1200.0 * std::log2(rightMeasured / expected);
    std::cout << "PSOLA stereo: left=" << leftMeasured << " Hz (" << leftError
              << " cents), right=" << rightMeasured << " Hz (" << rightError
              << " cents)" << std::endl;
    return std::abs(leftError) <= 35.0 && std::abs(rightError) <= 35.0;
}

bool testProductionAdapter()
{
    constexpr double sampleRate = 48000.0;
    constexpr double frequency = 220.0;
    constexpr int totalSamples = 65536;
    constexpr int blockSizes[] = {64, 128, 256, 512, 1024, 192, 768, 48, 320};

    PsolaRealtimeAdapter adapter;
    adapter.prepare(sampleRate);
    juce::AudioBuffer<float> block(2, 1024);
    const int period = PsolaRealtimeAdapter::periodFromFrequency(sampleRate, frequency);
    if (period != 218
        || PsolaRealtimeAdapter::periodFromFrequency(44100.0, 220.0) != 200
        || PsolaRealtimeAdapter::periodFromFrequency(96000.0, 220.0) != 436)
        return false;

    double phase = 0.0;
    const double step = juce::MathConstants<double>::twoPi * frequency / sampleRate;
    std::vector<float> leftCaptured, rightCaptured;
    leftCaptured.reserve(totalSamples - 8192);
    rightCaptured.reserve(totalSamples - 8192);
    int processed = 0;
    size_t blockIndex = 0;
    while (processed < totalSamples)
    {
        const int count = std::min(blockSizes[blockIndex++ % (sizeof(blockSizes) / sizeof(blockSizes[0]))],
                                   totalSamples - processed);
        for (int i = 0; i < count; ++i)
        {
            const float sample = 0.35f * static_cast<float>(std::sin(phase));
            block.setSample(0, i, sample);
            block.setSample(1, i, 0.7f * sample);
            phase += step;
            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;
        }
        // JUCE buffers retain their maximum size; expose the current host block
        // by processing a view-sized temporary without allocating in the adapter.
        juce::AudioBuffer<float> hostBlock(block.getArrayOfWritePointers(), 2, count);
        adapter.process(hostBlock, period, -5.0);
        for (int i = 0; i < count; ++i)
        {
            const float outL = hostBlock.getSample(0, i);
            const float outR = hostBlock.getSample(1, i);
            if (!std::isfinite(outL) || !std::isfinite(outR))
                return false;
            if (processed + i >= 8192)
            {
                leftCaptured.push_back(outL);
                rightCaptured.push_back(outR);
            }
        }
        processed += count;
    }

    const double expected = frequency * std::pow(2.0, -5.0 / 12.0);
    const double measuredLeft = estimateFrequency(leftCaptured, sampleRate, expected);
    const double measuredRight = estimateFrequency(rightCaptured, sampleRate, expected);
    const double errorLeft = 1200.0 * std::log2(measuredLeft / expected);
    const double errorRight = 1200.0 * std::log2(measuredRight / expected);

    std::cout << "Production adapter: sampleRate=" << sampleRate
              << ", period=" << period
              << ", left=" << measuredLeft << " Hz (" << errorLeft << " cents)"
              << ", right=" << measuredRight << " Hz (" << errorRight << " cents)"
              << ", FIFO delay=" << adapter.getLatencySamples()
              << ", underflows=" << adapter.getUnderflows()
              << ", overflows=" << adapter.getOverflows() << std::endl;
    return adapter.getUnderflows() == 0 && adapter.getOverflows() == 0
        && adapter.getLatencySamples() == 1024
        && std::abs(errorLeft) <= 35.0 && std::abs(errorRight) <= 35.0;
}

int measureProductionAdapterImpulsePeak()
{
    constexpr double sampleRate = 48000.0;
    constexpr int totalSamples = 65536;
    constexpr int blockSize = 256;
    PsolaRealtimeAdapter adapter;
    adapter.prepare(sampleRate);
    juce::AudioBuffer<float> storage(2, blockSize);
    int peakIndex = -1;
    float peak = 0.0f;
    int globalIndex = 0;
    while (globalIndex < totalSamples)
    {
        const int count = std::min(blockSize, totalSamples - globalIndex);
        storage.clear();
        if (globalIndex == 0)
            storage.setSample(0, 0, 1.0f);
        juce::AudioBuffer<float> hostBlock(storage.getArrayOfWritePointers(), 2, count);
        adapter.process(hostBlock, 218, -5.0);
        for (int i = 0; i < count; ++i)
        {
            const float value = std::abs(hostBlock.getSample(0, i));
            if (value > peak)
            {
                peak = value;
                peakIndex = globalIndex + i;
            }
        }
        globalIndex += count;
    }
    std::cout << "Production adapter impulse peak index=" << peakIndex
              << " at 48 kHz, peak=" << peak << std::endl;
    return peakIndex;
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
    const int adapterImpulsePeak = measureProductionAdapterImpulsePeak();
    const bool stereoPsola = testStereoPsola();
    const bool variableBlocks = testVariableHostBlocks();
    const bool productionAdapter = testProductionAdapter();

    if (!shiftedVoice1024 || !shiftedVoice2048 || !shiftedVoice4096 || !unshiftedLowVoice || impulsePeak < 0 || adapterImpulsePeak < 0 || !stereoPsola || !variableBlocks || !productionAdapter)
    {
        std::cerr << "PSOLA quality test failed: shifted2048=" << shiftedVoice2048
                  << ", shifted1024=" << shiftedVoice1024 << ", shifted2048=" << shiftedVoice2048
                  << ", shifted4096=" << shiftedVoice4096 << ", unshiftedLowVoice=" << unshiftedLowVoice
                  << ", impulsePeak=" << impulsePeak << ", adapterImpulsePeak=" << adapterImpulsePeak << ", stereoPsola=" << stereoPsola
                  << ", variableBlocks=" << variableBlocks << ", productionAdapter=" << productionAdapter << std::endl;
        return 1;
    }

    std::cout << "PSOLA frequency and impulse tests passed." << std::endl;
    return 0;
}
