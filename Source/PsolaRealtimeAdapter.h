#pragma once

#include "sola.h"

#include <array>
#include <cmath>
#include <memory>

class PsolaRealtimeAdapter
{
public:
    static constexpr int frameSize = 1024;
    static constexpr int ringSize = 8192;
    static constexpr int maxChannels = 2;

    PsolaRealtimeAdapter() = default;
    PsolaRealtimeAdapter(const PsolaRealtimeAdapter&) = delete;
    PsolaRealtimeAdapter& operator=(const PsolaRealtimeAdapter&) = delete;

    // Call only from prepareToPlay / a non-audio thread. All PSOLA and FIFO
    // storage is allocated here, never from process().
    void prepare(double sampleRate)
    {
        const double safeRate = sampleRate > 0.0 ? sampleRate : 44100.0;
        maxPeriod = static_cast<int>(std::ceil(safeRate / 65.0));
        processor = std::make_unique<TD_pitch_shift::Sola>(
            maxChannels, frameSize, maxPeriod);
        frame.setSize(maxChannels, frameSize, false, true, false);
        reset();
    }

    void release() noexcept
    {
        processor.reset();
        frame.setSize(0, 0);
        reset();
    }

    void reset() noexcept
    {
        for (auto& channel : inputFrame)
            channel.fill(0.0f);
        for (auto& channel : outputRing)
            channel.fill(0.0f);
        frameFill = 0;
        periodFrequencySum = 0.0;
        semitoneSum = 0.0;
        parameterSampleCount = 0;
        readOutput = writeOutput = outputCount = 0;
        // One frame of startup silence keeps the FIFO deterministic.
        for (int i = 0; i < frameSize; ++i)
            pushOutput(0, 0.0f), pushOutput(1, 0.0f);
        underflows = overflows = 0;
    }

    // period is measured in samples at the plugin's full sample rate.
    // semitones follows the tested convention: negative values lower pitch.
    void process(juce::AudioBuffer<float>& buffer, double period,
                 double semitones) noexcept
    {
        if (processor == nullptr || buffer.getNumSamples() <= 0)
            return;

        const int channels = juce::jlimit(1, maxChannels, buffer.getNumChannels());
        auto* left = buffer.getWritePointer(0);
        auto* right = channels > 1 ? buffer.getWritePointer(1) : nullptr;
        const double safePeriod = juce::jlimit(1.0, static_cast<double>(maxPeriod), period);
        const double safeShift = juce::jlimit(-12.0, 12.0, semitones);

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            inputFrame[0][static_cast<size_t>(frameFill)] = left[i];
            inputFrame[1][static_cast<size_t>(frameFill)] = right != nullptr ? right[i] : left[i];

            // Host blocks can split a PSOLA frame. Accumulate the control
            // estimates over the samples that actually form this frame,
            // rather than applying only the values from its final host block.
            periodFrequencySum += 1.0 / static_cast<double>(safePeriod);
            semitoneSum += safeShift;
            ++parameterSampleCount;

            const float outLeft = popOutput(0);
            const float outRight = popOutput(1);
            left[i] = outLeft;
            if (right != nullptr)
                right[i] = outRight;

            if (++frameFill == frameSize)
            {
                frame.copyFrom(0, 0, inputFrame[0].data(), frameSize);
                frame.copyFrom(1, 0, inputFrame[1].data(), frameSize);
                const double averageFrequency = periodFrequencySum
                    / static_cast<double>(parameterSampleCount);
                const double framePeriod = juce::jlimit(1.0, static_cast<double>(maxPeriod),
                    1.0 / averageFrequency);
                const double frameShift = semitoneSum
                    / static_cast<double>(parameterSampleCount);
                lastProcessedPeriod = static_cast<int>(std::lround(framePeriod));
                lastProcessedSemitones = frameShift;
                processor->Process(frame, framePeriod, frameShift);
                periodFrequencySum = 0.0;
                semitoneSum = 0.0;
                parameterSampleCount = 0;
                for (int sample = 0; sample < frameSize; ++sample)
                {
                    pushOutput(0, frame.getSample(0, sample));
                    pushOutput(1, frame.getSample(1, sample));
                }
                frameFill = 0;
            }
        }
    }

    int getLatencySamples() const noexcept { return frameSize; }
    int getUnderflows() const noexcept { return underflows; }
    int getOverflows() const noexcept { return overflows; }
    int getLastProcessedPeriod() const noexcept { return lastProcessedPeriod; }
    double getLastProcessedSemitones() const noexcept { return lastProcessedSemitones; }

    static double precisePeriodFromFrequency(double sampleRate, double frequency) noexcept
    {
        if (!(sampleRate > 0.0) || !(frequency >= 65.0) || !std::isfinite(frequency))
            return 0.0;
        return sampleRate / frequency;
    }

    static int periodFromFrequency(double sampleRate, double frequency) noexcept
    {
        const double period = precisePeriodFromFrequency(sampleRate, frequency);
        return period > 0.0 ? static_cast<int>(std::lround(period)) : 0;
    }

private:
    void pushOutput(int channel, float value) noexcept
    {
        if (outputCount >= ringSize)
        {
            ++overflows;
            return;
        }
        outputRing[static_cast<size_t>(channel)][static_cast<size_t>(writeOutput)] = value;
        if (channel == maxChannels - 1)
            writeOutput = (writeOutput + 1) % ringSize;
        if (channel == maxChannels - 1)
            ++outputCount;
    }

    float popOutput(int channel) noexcept
    {
        if (outputCount <= 0)
        {
            ++underflows;
            return 0.0f;
        }
        const float value = outputRing[static_cast<size_t>(channel)][static_cast<size_t>(readOutput)];
        if (channel == maxChannels - 1)
        {
            readOutput = (readOutput + 1) % ringSize;
            --outputCount;
        }
        return value;
    }

    std::unique_ptr<TD_pitch_shift::Sola> processor;
    juce::AudioBuffer<float> frame;
    std::array<std::array<float, frameSize>, maxChannels> inputFrame {};
    std::array<std::array<float, ringSize>, maxChannels> outputRing {};
    int maxPeriod = 1;
    int frameFill = 0;
    double periodFrequencySum = 0.0;
    double semitoneSum = 0.0;
    int parameterSampleCount = 0;
    int lastProcessedPeriod = 0;
    double lastProcessedSemitones = 0.0;
    int readOutput = 0, writeOutput = 0, outputCount = 0;
    int underflows = 0, overflows = 0;
};
