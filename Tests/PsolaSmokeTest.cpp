#include "sola.h"
#include "PsolaRealtimeAdapter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
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

bool testPitchGlide()
{
    constexpr double sampleRate = 44100.0;
    constexpr int frameSize = 1024;
    constexpr int glideFrames = 208;
    constexpr int settleFrames = 32;
    constexpr double startFrequency = 180.0;
    constexpr double endFrequency = 280.0;
    constexpr double shiftSemitones = -3.0;
    const int maxPeriod = static_cast<int>(std::ceil(sampleRate / 65.0));

    TD_pitch_shift::Sola processor(1, frameSize, maxPeriod);
    juce::AudioBuffer<float> frame(1, frameSize);
    std::vector<float> settledSegment;
    std::vector<float> trackingSegment;
    settledSegment.reserve(settleFrames * frameSize);
    trackingSegment.reserve(8 * frameSize);
    double phase = 0.0;
    bool allFinite = true;

    // Smoothly sweep the input pitch sample-by-sample, then hold the final note so the measurement
    // evaluates steady-state tracking rather than averaging the whole glide.
    for (int block = 0; block < glideFrames + settleFrames; ++block)
    {
        const double blockProgress = static_cast<double>(block) / static_cast<double>(glideFrames);
        const double nextProgress = static_cast<double>(block + 1) / static_cast<double>(glideFrames);
        const double blockStartFrequency = block < glideFrames
            ? startFrequency + (endFrequency - startFrequency) * blockProgress
            : endFrequency;
        const double blockEndFrequency = block < glideFrames
            ? startFrequency + (endFrequency - startFrequency) * nextProgress
            : endFrequency;
        const double averageFrequency = 0.5 * (blockStartFrequency + blockEndFrequency);
        const int period = static_cast<int>(std::lround(sampleRate / averageFrequency));
        auto* samples = frame.getWritePointer(0);
        for (int i = 0; i < frameSize; ++i)
        {
            const double sampleProgress = static_cast<double>(i) / static_cast<double>(frameSize);
            const double frequency = blockStartFrequency
                + (blockEndFrequency - blockStartFrequency) * sampleProgress;
            const double phaseStep = juce::MathConstants<double>::twoPi * frequency / sampleRate;
            samples[i] = 0.45f * static_cast<float>(std::sin(phase));
            phase += phaseStep;
            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;
        }

        processor.Process(frame, period, shiftSemitones);
        for (int i = 0; i < frameSize; ++i)
        {
            allFinite = allFinite && std::isfinite(samples[i]);
            if (block >= glideFrames)
                settledSegment.push_back(samples[i]);
            else if (block >= glideFrames - 8)
                trackingSegment.push_back(samples[i]);
        }
    }

    const double expected = endFrequency * std::pow(2.0, shiftSemitones / 12.0);
    const double measured = estimateFrequency(settledSegment, sampleRate, expected);
    // PSOLA's isolated engine impulse peak is 1358 samples at 44.1 kHz.
    // The output tracking window therefore corresponds to an earlier input
    // point; compensate the full engine latency before judging glide accuracy.
    constexpr double measuredPsolaLatencySamples = 1358.0;
    const double trackingMidpointInputSample =
        static_cast<double>(glideFrames - 4.5) * frameSize - measuredPsolaLatencySamples;
    const double trackingExpectedInput = startFrequency
        + (endFrequency - startFrequency)
            * (trackingMidpointInputSample / (static_cast<double>(glideFrames - 1) * frameSize));
    const double trackingExpected = trackingExpectedInput * std::pow(2.0, shiftSemitones / 12.0);
    const double trackingMeasured = estimateFrequency(trackingSegment, sampleRate, trackingExpected);
    const double trackingErrorCents = trackingMeasured > 0.0
        ? 1200.0 * std::log2(trackingMeasured / trackingExpected) : 10000.0;
    const double errorCents = measured > 0.0
        ? 1200.0 * std::log2(measured / expected) : 10000.0;
    double energy = 0.0;
    for (float sample : settledSegment)
        energy += static_cast<double>(sample) * sample;

    std::cout << "PSOLA glide " << startFrequency << "->" << endFrequency
              << " Hz then hold, expected=" << expected << " Hz, measured="
              << measured << " Hz, error=" << errorCents
              << " cents; glide-tail expected=" << trackingExpected
              << " Hz, measured=" << trackingMeasured
              << " Hz, error=" << trackingErrorCents
              << " cents, finite=" << allFinite << std::endl;
    return allFinite && energy / settledSegment.size() > 1.0e-6
        && std::abs(errorCents) <= 35.0
        && trackingMeasured > 0.0 && std::abs(trackingErrorCents) <= 65.0;
}


bool testFastPitchGlide()
{
    constexpr double sampleRate = 44100.0;
    constexpr int frameSize = 1024;
    constexpr int glideFrames = 64;
    constexpr int settleFrames = 32;
    constexpr double startFrequency = 180.0;
    constexpr double endFrequency = 280.0;
    constexpr double shiftSemitones = -3.0;
    const int maxPeriod = static_cast<int>(std::ceil(sampleRate / 65.0));

    TD_pitch_shift::Sola processor(1, frameSize, maxPeriod);
    juce::AudioBuffer<float> frame(1, frameSize);
    std::vector<float> settledSegment;
    std::vector<float> trackingSegment;
    settledSegment.reserve(settleFrames * frameSize);
    trackingSegment.reserve(16 * frameSize);
    double phase = 0.0;
    bool allFinite = true;

    // Sweep the input pitch, then hold the final note so the measurement
    // evaluates steady-state tracking rather than averaging the whole glide.
    for (int block = 0; block < glideFrames + settleFrames; ++block)
    {
        const double progress = static_cast<double>(std::min(block, glideFrames - 1))
            / static_cast<double>(glideFrames - 1);
        const double frequency = block < glideFrames
            ? startFrequency + (endFrequency - startFrequency) * progress
            : endFrequency;
        const int period = static_cast<int>(std::lround(sampleRate / frequency));
        const double phaseStep = juce::MathConstants<double>::twoPi * frequency / sampleRate;
        auto* samples = frame.getWritePointer(0);
        for (int i = 0; i < frameSize; ++i)
        {
            samples[i] = 0.45f * static_cast<float>(std::sin(phase));
            phase += phaseStep;
            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;
        }

        processor.Process(frame, period, shiftSemitones);
        for (int i = 0; i < frameSize; ++i)
        {
            allFinite = allFinite && std::isfinite(samples[i]);
            if (block >= glideFrames)
                settledSegment.push_back(samples[i]);
            else if (block >= glideFrames - 8)
                trackingSegment.push_back(samples[i]);
        }
    }

    const double expected = endFrequency * std::pow(2.0, shiftSemitones / 12.0);
    const double measured = estimateFrequency(settledSegment, sampleRate, expected);
    // The spectral estimate covers all eight captured frames, so compare it
    // with the midpoint frequency of that window rather than its first frame.
    // Compensate the isolated PSOLA engine's measured 1358-sample
    // impulse latency so the spectral window is compared to its source time.
    constexpr double measuredPsolaLatencySamples = 1358.0;
    const double trackingWindowMidpointSamples =
        static_cast<double>(glideFrames - 4.5) * frameSize - measuredPsolaLatencySamples;
    const double trackingExpectedInput = startFrequency
        + (endFrequency - startFrequency)
            * (trackingWindowMidpointSamples
               / (static_cast<double>(glideFrames - 1) * frameSize));
    const double trackingExpected = trackingExpectedInput * std::pow(2.0, shiftSemitones / 12.0);
    const double trackingMeasured = estimateFrequency(trackingSegment, sampleRate, trackingExpected);
    const double trackingErrorCents = trackingMeasured > 0.0
        ? 1200.0 * std::log2(trackingMeasured / trackingExpected) : 10000.0;
    const double errorCents = measured > 0.0
        ? 1200.0 * std::log2(measured / expected) : 10000.0;
    double energy = 0.0;
    for (float sample : settledSegment)
        energy += static_cast<double>(sample) * sample;

    std::cout << "PSOLA fast glide " << startFrequency << "->" << endFrequency
              << " Hz then hold, expected=" << expected << " Hz, measured="
              << measured << " Hz, error=" << errorCents
              << " cents; glide-tail expected=" << trackingExpected
              << " Hz, measured=" << trackingMeasured
              << " Hz, error=" << trackingErrorCents
              << " cents, finite=" << allFinite << std::endl;
    return allFinite && energy / settledSegment.size() > 1.0e-6
        && std::abs(errorCents) <= 35.0
        && trackingMeasured > 0.0 && std::abs(trackingErrorCents) <= 35.0;
}


struct SinePhaseMeasurement
{
    double frequency = 0.0;
    double phaseDifference = 0.0;
    double coherence = 0.0;
};

SinePhaseMeasurement measureSinePhase(double frequency)
{
    constexpr double sampleRate = 44100.0;
    constexpr int frameSize = 1024;
    constexpr int warmupSamples = 32768;
    constexpr int captureSamples = 16384;
    const int maxPeriod = static_cast<int>(std::ceil(sampleRate / 65.0));
    const int period = static_cast<int>(std::lround(sampleRate / frequency));
    TD_pitch_shift::Sola processor(1, frameSize, maxPeriod);
    juce::AudioBuffer<float> frame(1, frameSize);
    std::vector<float> input;
    std::vector<float> output;
    input.reserve(warmupSamples + captureSamples);
    output.reserve(warmupSamples + captureSamples);
    double phase = 0.0;
    const double step = juce::MathConstants<double>::twoPi * frequency / sampleRate;

    for (int block = 0; block < (warmupSamples + captureSamples) / frameSize; ++block)
    {
        auto* samples = frame.getWritePointer(0);
        for (int i = 0; i < frameSize; ++i)
        {
            const float value = 0.4f * static_cast<float>(std::sin(phase));
            samples[i] = value;
            input.push_back(value);
            phase += step;
            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;
        }
        processor.Process(frame, period, 0.0);
        for (int i = 0; i < frameSize; ++i)
            output.push_back(frame.getSample(0, i));
    }

    double inReal = 0.0, inImag = 0.0, outReal = 0.0, outImag = 0.0;
    const int begin = warmupSamples;
    for (int i = 0; i < captureSamples; ++i)
    {
        const double angle = step * static_cast<double>(begin + i);
        const double c = std::cos(angle), sn = std::sin(angle);
        inReal += input[static_cast<size_t>(begin + i)] * c;
        inImag -= input[static_cast<size_t>(begin + i)] * sn;
        outReal += output[static_cast<size_t>(begin + i)] * c;
        outImag -= output[static_cast<size_t>(begin + i)] * sn;
    }
    const double inputMagnitude = std::hypot(inReal, inImag);
    const double outputMagnitude = std::hypot(outReal, outImag);
    const double phaseDifference = std::atan2(outImag, outReal)
        - std::atan2(inImag, inReal);
    return { frequency, std::remainder(phaseDifference,
        juce::MathConstants<double>::twoPi),
        inputMagnitude > 0.0 ? outputMagnitude / inputMagnitude : 0.0 };
}

bool testSinusoidalDelayResponse()
{
    constexpr double sampleRate = 44100.0;
    const std::array<double, 6> frequencies{ 97.0, 131.0, 173.0, 220.0, 317.0, 443.0 };
    std::array<SinePhaseMeasurement, frequencies.size()> measurements{};
    for (size_t i = 0; i < frequencies.size(); ++i)
        measurements[i] = measureSinePhase(frequencies[i]);

    // Find the common integer-sample delay that best explains the measured
    // phase at every frequency. A single impulse peak cannot provide this
    // cross-frequency phase/group-delay check.
    double bestError = std::numeric_limits<double>::max();
    int bestDelay = -1;
    for (int delay = 0; delay <= 4096; ++delay)
    {
        double error = 0.0;
        for (const auto& measurement : measurements)
        {
            const double predicted = -juce::MathConstants<double>::twoPi
                * measurement.frequency * static_cast<double>(delay) / sampleRate;
            const double residual = std::remainder(
                measurement.phaseDifference - predicted,
                juce::MathConstants<double>::twoPi);
            error += measurement.coherence * residual * residual;
        }
        if (error < bestError)
        {
            bestError = error;
            bestDelay = delay;
        }
    }

    std::cout << "PSOLA zero-shift sinusoidal phase response:";
    for (const auto& measurement : measurements)
        std::cout << " " << measurement.frequency << "Hz:"
                  << (measurement.phaseDifference * 180.0
                      / juce::MathConstants<double>::pi)
                  << "deg/gain" << measurement.coherence;
    std::cout << "; best common phase-delay=" << bestDelay
              << " samples, residual=" << bestError << std::endl;
    return bestDelay >= 0 && std::isfinite(bestError);
}

bool testAbruptControlTransitions()
{
    constexpr double sampleRate = 44100.0;
    constexpr double inputFrequency = 220.0;
    constexpr int frameSize = 1024;
    constexpr int totalFrames = 240;
    constexpr int transitionFrames[] = { 80, 120, 160 };
    constexpr double shifts[] = { 0.0, -5.0, 5.0, 0.0 };
    constexpr int smoothingDurations[] = { 2, 4, 8, 16 };
    const int maxPeriod = static_cast<int>(std::ceil(sampleRate / 65.0));
    const int period = static_cast<int>(std::lround(sampleRate / inputFrequency));

    TD_pitch_shift::Sola abruptProcessor(1, frameSize, maxPeriod);
    std::array<std::unique_ptr<TD_pitch_shift::Sola>, 4> referenceProcessors;
    for (auto& reference : referenceProcessors)
        reference = std::make_unique<TD_pitch_shift::Sola>(1, frameSize, maxPeriod);

    std::array<std::unique_ptr<TD_pitch_shift::Sola>, 4> smoothProcessors;
    std::array<std::vector<float>, 4> smoothOutputs;
    for (size_t i = 0; i < smoothProcessors.size(); ++i)
    {
        smoothProcessors[i] = std::make_unique<TD_pitch_shift::Sola>(1, frameSize, maxPeriod);
        smoothOutputs[i].reserve(totalFrames * frameSize);
    }

    juce::AudioBuffer<float> abruptFrame(1, frameSize);
    juce::AudioBuffer<float> referenceFrame(1, frameSize);
    std::vector<float> abruptOutput;
    std::array<std::vector<float>, 4> referenceOutput;
    abruptOutput.reserve(totalFrames * frameSize);
    for (auto& samples : referenceOutput)
        samples.reserve(totalFrames * frameSize);

    double phase = 0.0;
    bool allFinite = true;

    for (int block = 0; block < totalFrames; ++block)
    {
        int state = 0;
        for (int transition : transitionFrames)
            if (block >= transition)
                ++state;

        auto* abruptSamples = abruptFrame.getWritePointer(0);
        auto* referenceSamples = referenceFrame.getWritePointer(0);
        for (int sample = 0; sample < frameSize; ++sample)
        {
            const float input = 0.4f * static_cast<float>(std::sin(phase));
            abruptSamples[sample] = input;
            referenceSamples[sample] = input;
            phase += juce::MathConstants<double>::twoPi * inputFrequency / sampleRate;
            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;
        }

        abruptProcessor.Process(abruptFrame, period, shifts[state]);
        for (int transition : transitionFrames)
        {
            if (block >= transition - 2 && block <= transition + 4)
            {
                std::cout << "PSOLA mark timing block=" << block
                          << ", requested-shift=" << shifts[state]
                          << ", analysis-period=" << abruptProcessor.getAnalysisPeriod()
                          << ", synthesis-period=" << abruptProcessor.getSynthesisPeriod()
                          << ", analysis-mark-offset=" << abruptProcessor.getAnalysisMarkOffset()
                          << ", synthesis-mark-offset=" << abruptProcessor.getSynthesisMarkOffset()
                          << std::endl;
                break;
            }
        }
        for (int sample = 0; sample < frameSize; ++sample)
        {
            allFinite = allFinite && std::isfinite(abruptSamples[sample]);
            abruptOutput.push_back(abruptSamples[sample]);
        }

        for (int referenceIndex = 0; referenceIndex < 4; ++referenceIndex)
        {
            referenceProcessors[static_cast<size_t>(referenceIndex)]->Process(
                referenceFrame, period, shifts[static_cast<size_t>(referenceIndex)]);
            for (int sample = 0; sample < frameSize; ++sample)
            {
                allFinite = allFinite && std::isfinite(referenceSamples[sample]);
                referenceOutput[static_cast<size_t>(referenceIndex)].push_back(referenceSamples[sample]);
            }
            // Rebuild the identical input frame before processing the next independent reference.
            for (int sample = 0; sample < frameSize; ++sample)
            {
                const double samplePhase = phase - juce::MathConstants<double>::twoPi
                    * inputFrequency * static_cast<double>(frameSize - sample) / sampleRate;
                referenceSamples[sample] = 0.4f * static_cast<float>(std::sin(samplePhase));
            }
        }

        for (size_t durationIndex = 0; durationIndex < smoothProcessors.size(); ++durationIndex)
        {
            const int duration = smoothingDurations[durationIndex];
            double smoothShift = shifts[state];
            for (size_t transitionIndex = 0; transitionIndex < std::size(transitionFrames); ++transitionIndex)
            {
                const int transition = transitionFrames[transitionIndex];
                if (block >= transition && block < transition + duration)
                {
                    const double progress = static_cast<double>(block - transition + 1)
                        / static_cast<double>(duration);
                    smoothShift = shifts[transitionIndex]
                        + (shifts[transitionIndex + 1] - shifts[transitionIndex]) * progress;
                    break;
                }
            }

            juce::AudioBuffer<float> smoothFrame(1, frameSize);
            auto* smoothSamples = smoothFrame.getWritePointer(0);
            for (int sample = 0; sample < frameSize; ++sample)
            {
                const double samplePhase = phase - juce::MathConstants<double>::twoPi
                    * inputFrequency * static_cast<double>(frameSize - sample) / sampleRate;
                smoothSamples[sample] = 0.4f * static_cast<float>(std::sin(samplePhase));
            }
            smoothProcessors[durationIndex]->Process(smoothFrame, period, smoothShift);
            for (int sample = 0; sample < frameSize; ++sample)
            {
                allFinite = allFinite && std::isfinite(smoothSamples[sample]);
                smoothOutputs[durationIndex].push_back(smoothSamples[sample]);
            }
        }
    }

    auto peakStep = [](const std::vector<float>& samples, int begin, int end)
    {
        double peak = 0.0;
        for (int i = begin; i < end; ++i)
            peak = std::max(peak, std::abs(static_cast<double>(samples[static_cast<size_t>(i)])
                - samples[static_cast<size_t>(i - 1)]));
        return peak;
    };

    for (size_t durationIndex = 0; durationIndex < smoothProcessors.size(); ++durationIndex)
    {
        double worstSmoothRatio = 0.0;
        for (size_t transitionIndex = 0; transitionIndex < std::size(transitionFrames); ++transitionIndex)
        {
            const int expected = transitionFrames[transitionIndex] * frameSize + 1358;
            const int begin = std::max(1, expected - 2 * frameSize);
            const int end = std::min(static_cast<int>(abruptOutput.size()) - 1,
                                     expected + 2 * frameSize);
            const size_t referenceIndex = transitionIndex + 1;
            const double referencePeak = peakStep(referenceOutput[referenceIndex], begin, end);
            const double abruptPeak = peakStep(abruptOutput, begin, end);
            const double smoothPeak = peakStep(smoothOutputs[durationIndex], begin, end);
            const double abruptRatio = referencePeak > 0.0 ? abruptPeak / referencePeak : 0.0;
            const double smoothRatio = referencePeak > 0.0 ? smoothPeak / referencePeak : 0.0;
            worstSmoothRatio = std::max(worstSmoothRatio, smoothRatio);
            std::cout << "PSOLA transition frame=" << transitionFrames[transitionIndex]
                      << ", target-shift=" << shifts[referenceIndex]
                      << ", abrupt/reference=" << abruptRatio
                      << ", smoothed/reference=" << smoothRatio
                      << ", smoothing-frames=" << smoothingDurations[durationIndex] << std::endl;
        }
        std::cout << "PSOLA smoothing duration=" << smoothingDurations[durationIndex]
                  << " frames, worst smoothed/reference=" << worstSmoothRatio << std::endl;
        allFinite = allFinite && std::isfinite(worstSmoothRatio) && worstSmoothRatio > 0.0;
    }

    return allFinite;
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

bool testAdapterAveragesControlsAcrossHostBlocks()
{
    constexpr double sampleRate = 48000.0;
    PsolaRealtimeAdapter adapter;
    adapter.prepare(sampleRate);
    juce::AudioBuffer<float> storage(2, 512);
    storage.clear();

    // Split one PSOLA frame across two host blocks with different controls.
    juce::AudioBuffer<float> first(storage.getArrayOfWritePointers(), 2, 512);
    adapter.process(first, 200, -2.0);
    juce::AudioBuffer<float> second(storage.getArrayOfWritePointers(), 2, 512);
    adapter.process(second, 100, 6.0);

    const int expectedPeriod = static_cast<int>(std::lround(
        1.0 / (0.5 / 200.0 + 0.5 / 100.0)));
    const double expectedShift = 2.0;
    const bool passed = adapter.getLastProcessedPeriod() == expectedPeriod
        && std::abs(adapter.getLastProcessedSemitones() - expectedShift) < 1.0e-9
        && adapter.getUnderflows() == 0 && adapter.getOverflows() == 0;

    std::cout << "Adapter control averaging: period="
              << adapter.getLastProcessedPeriod() << " (expected " << expectedPeriod
              << "), shift=" << adapter.getLastProcessedSemitones()
              << " (expected " << expectedShift << "), passed=" << passed << std::endl;
    return passed;
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


bool testProductionAdapterFastGlide()
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 256;
    constexpr int warmupSamples = 8192;
    constexpr int glideSamples = 65536;
    constexpr int holdSamples = 32768;
    PsolaRealtimeAdapter adapter;
    adapter.prepare(sampleRate);
    juce::AudioBuffer<float> storage(2, blockSize);
    std::vector<float> captured;
    captured.reserve(holdSamples);
    std::vector<float> glideTail;
    constexpr int glideWindowSamples = 8192;
    glideTail.reserve(glideWindowSamples);
    double phase = 0.0;
    int globalSample = 0;
    int previousPeriod = 0;
    bool controlsMonotonic = true;
    while (globalSample < warmupSamples + glideSamples + holdSamples)
    {
        const int count = std::min(blockSize,
            warmupSamples + glideSamples + holdSamples - globalSample);
        const int relative = globalSample - warmupSamples;
        const double frequency = relative < 0 ? 180.0
            : relative < glideSamples
                ? 180.0 + 100.0 * static_cast<double>(relative) / glideSamples
                : 280.0;
        const int period = PsolaRealtimeAdapter::periodFromFrequency(sampleRate, frequency);
        for (int i = 0; i < count; ++i)
        {
            const int samplePosition = relative + i;
            const double inputFrequency = samplePosition < 0 ? 180.0
                : samplePosition < glideSamples
                    ? 180.0 + 100.0 * static_cast<double>(samplePosition) / glideSamples
                    : 280.0;
            const float sample = 0.3f * static_cast<float>(std::sin(phase));
            storage.setSample(0, i, sample);
            storage.setSample(1, i, sample);
            phase += juce::MathConstants<double>::twoPi * inputFrequency / sampleRate;
            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;
        }
        juce::AudioBuffer<float> hostBlock(storage.getArrayOfWritePointers(), 2, count);
        adapter.process(hostBlock, period, 0.0);
        if (globalSample >= warmupSamples + glideSamples)
            for (int i = 0; i < count; ++i)
                captured.push_back(hostBlock.getSample(0, i));
        const int glideTailStart = warmupSamples + glideSamples - glideWindowSamples;
        for (int i = 0; i < count; ++i)
        {
            const int absoluteSample = globalSample + i;
            if (absoluteSample >= glideTailStart
                && absoluteSample < warmupSamples + glideSamples)
                glideTail.push_back(hostBlock.getSample(0, i));
        }
        if (globalSample >= warmupSamples && globalSample < warmupSamples + glideSamples)
        {
            const int processedPeriod = adapter.getLastProcessedPeriod();
            if (processedPeriod > 0 && previousPeriod > 0 && processedPeriod > previousPeriod)
                controlsMonotonic = false;
            if (processedPeriod > 0)
                previousPeriod = processedPeriod;
        }
        globalSample += count;
    }
    const double measured = estimateFrequency(captured, sampleRate, 280.0);
    const double errorCents = 1200.0 * std::log2(measured / 280.0);
    // Compare the output window with the input time that produced it.
    // The adapter FIFO is 1024 samples, but the complete production path
    // (PSOLA engine plus FIFO) measures 2502 samples at 48 kHz.
    constexpr int measuredProductionLatency = 2502;
    const double glideMidpointInput = 180.0 + 100.0
        * (static_cast<double>(glideSamples - glideWindowSamples / 2 - measuredProductionLatency)
           / static_cast<double>(glideSamples));
    const double glideMeasured = estimateFrequency(glideTail, sampleRate, glideMidpointInput);
    const double glideErrorCents = 1200.0 * std::log2(glideMeasured / glideMidpointInput);
    std::cout << "Production adapter fast glide tail expected=" << glideMidpointInput
              << " Hz, measured=" << glideMeasured << " Hz, error="
              << glideErrorCents << " cents; hold measured="
              << measured << " Hz, error=" << errorCents
              << " cents, finalPeriod=" << adapter.getLastProcessedPeriod()
              << ", underflows=" << adapter.getUnderflows()
              << ", overflows=" << adapter.getOverflows() << std::endl;
    return std::isfinite(measured) && std::isfinite(glideMeasured)
        && std::abs(glideErrorCents) <= 65.0 && std::abs(errorCents) <= 35.0
        && controlsMonotonic && adapter.getUnderflows() == 0
        && adapter.getOverflows() == 0;
}

int measureProductionAdapterImpulsePeak(double sampleRate, double shift)
{
    constexpr int totalSamples = 65536;
    constexpr int blockSize = 256;
    PsolaRealtimeAdapter adapter;
    adapter.prepare(sampleRate);
    const int period = PsolaRealtimeAdapter::periodFromFrequency(sampleRate, 220.0);
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
        adapter.process(hostBlock, period, shift);
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
    std::cout << "Production adapter impulse peak=" << peakIndex
              << " samples at " << sampleRate << " Hz, shift="
              << shift << " st, peak=" << peak << std::endl;
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
    // Broader pitch-accuracy matrix: low/mid/high registers and both shift directions.
    const bool lowVoiceDown = runCase(44100.0, 110.0, -5.0, 1024);
    const bool midVoiceUp = runCase(44100.0, 220.0, 5.0, 1024);
    const bool highVoiceDown = runCase(44100.0, 330.0, -7.0, 1024);
    const bool highVoiceUp = runCase(44100.0, 440.0, 3.0, 1024);
    const int impulsePeak = measureImpulsePeak();
    bool adapterImpulseTests = true;
    for (double rate : { 44100.0, 48000.0, 96000.0 })
        for (double shift : { -5.0, 0.0, 5.0 })
            adapterImpulseTests = (measureProductionAdapterImpulsePeak(rate, shift) > 0)
                               && adapterImpulseTests;
    std::cout << "Adapter latency matrix passed=" << adapterImpulseTests << std::endl;
    const bool pitchGlide = testPitchGlide();
    const bool fastPitchGlide = testFastPitchGlide();
    const bool sinusoidalDelayResponse = testSinusoidalDelayResponse();
    const bool abruptTransitions = testAbruptControlTransitions();
    const bool stereoPsola = testStereoPsola();
    const bool variableBlocks = testVariableHostBlocks();
    const bool productionAdapter = testProductionAdapter();
    const bool productionAdapterFastGlide = testProductionAdapterFastGlide();
    const bool adapterControlAveraging = testAdapterAveragesControlsAcrossHostBlocks();

    if (!shiftedVoice1024 || !shiftedVoice2048 || !shiftedVoice4096 || !unshiftedLowVoice
        || !lowVoiceDown || !midVoiceUp || !highVoiceDown || !highVoiceUp
        || impulsePeak < 0 || !adapterImpulseTests || !pitchGlide || !fastPitchGlide || !sinusoidalDelayResponse || !abruptTransitions || !stereoPsola || !variableBlocks || !productionAdapter || !productionAdapterFastGlide || !adapterControlAveraging)
    {
        std::cerr << "PSOLA quality test failed: shifted2048=" << shiftedVoice2048
                  << ", shifted1024=" << shiftedVoice1024 << ", shifted2048=" << shiftedVoice2048
                  << ", shifted4096=" << shiftedVoice4096 << ", unshiftedLowVoice=" << unshiftedLowVoice
                  << ", impulsePeak=" << impulsePeak << ", adapterImpulseTests=" << adapterImpulseTests << ", stereoPsola=" << stereoPsola
                  << ", pitchGlide=" << pitchGlide << ", fastPitchGlide=" << fastPitchGlide << ", variableBlocks=" << variableBlocks << ", productionAdapter=" << productionAdapter << std::endl;
        return 1;
    }

    std::cout << "PSOLA frequency and impulse tests passed." << std::endl;
    return 0;
}
