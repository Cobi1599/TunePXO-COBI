#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "PsolaRealtimeAdapter.h"
#include <cmath>

namespace {
constexpr float twoPi = juce::MathConstants<float>::twoPi;
constexpr int analysisDecimation = 2;
constexpr float pitchShiftDelayRange = 384.0f;
constexpr int minPitchLag = 12;
constexpr int maxPitchLag = 300;
constexpr float minimumCorrelation = 0.62f;
}

TunePXOCobiAudioProcessor::TunePXOCobiAudioProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "PARAMETERS", createParameterLayout())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout TunePXOCobiAudioProcessor::createParameterLayout()
{
    using Float = juce::AudioParameterFloat;
    using Choice = juce::AudioParameterChoice;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add(std::make_unique<Choice>("ENGINE", "Pitch Engine", juce::StringArray{ "Legacy", "PSOLA" }, 1));
    layout.add(std::make_unique<Choice>("MODE", "Tuning Style", juce::StringArray{ "Natural", "Hard Tune" }, 0));
    layout.add(std::make_unique<Choice>("KEY", "Key",
        juce::StringArray{ "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 0));
    layout.add(std::make_unique<Choice>("SCALE", "Scale",
        juce::StringArray{ "Major", "Minor", "Chromatic" }, 0));
    layout.add(std::make_unique<Float>("RETUNE", "Retune Speed",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 35.0f));
    layout.add(std::make_unique<Float>("HUMANIZE", "Humanize",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 20.0f));
    layout.add(std::make_unique<Float>("MIX", "Correction Mix",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 100.0f));
    return layout;
}

void TunePXOCobiAudioProcessor::prepareToPlay(double sampleRate, int)
{
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    psola.prepare(currentSampleRate);
    for (auto& channel : delayBuffers)
        channel.fill(0.0f);
    pitchBuffer.fill(0.0f);
    pitchWritePosition = samplesSinceAnalysis = audioSampleCounter = pitchSamplesWritten = delayWritePosition = 0;
    shifterPhase = 0.0f;
    smoothedMidiNote = -1.0f;
    smoothedCorrectionSemitones = 0.0f;
    smoothedPitchRatio = 1.0f;
    heldTargetPitchRatio = 1.0f;
    samplesSinceValidPitch = 0;
    detectedMidiNote.store(-1, std::memory_order_relaxed);
    detectedFrequency.store(0.0f, std::memory_order_relaxed);
}

void TunePXOCobiAudioProcessor::releaseResources() { psola.release(); }

int TunePXOCobiAudioProcessor::quantiseMidiNote(float midiNote, int key, int scale) const noexcept
{
    static constexpr bool major[12] = { true, false, true, false, true, true, false, true, false, true, false, true };
    static constexpr bool minor[12] = { true, false, true, true, false, true, false, true, true, false, true, false };
    const int nearest = (int)std::lround(midiNote);
    if (scale == 2) return nearest;
    const bool* allowed = scale == 1 ? minor : major;
    int best = nearest;
    float bestDistance = 1000.0f;
    for (int offset = -6; offset <= 6; ++offset) {
        const int candidate = nearest + offset;
        int pitchClass = (candidate - key) % 12;
        if (pitchClass < 0) pitchClass += 12;
        if (allowed[pitchClass] && std::abs(candidate - midiNote) < (float)bestDistance) {
            best = candidate;
            bestDistance = std::abs((float)candidate - midiNote);
        }
    }
    return best;
}

void TunePXOCobiAudioProcessor::analysePitch() noexcept
{
    // YIN-style difference function: prefer the first convincing period
    // instead of the strongest autocorrelation peak (which may be a harmonic).
    constexpr float yinThreshold = 0.16f;
    std::array<float, maxPitchLag + 1> difference {};
    std::array<float, maxPitchLag + 1> cumulativeMean {};

    float mean = 0.0f;
    for (float sample : pitchBuffer) mean += sample;
    mean /= (float)pitchWindowSize;

    float energy = 0.0f;
    for (float sample : pitchBuffer) {
        const float centred = sample - mean;
        energy += centred * centred;
    }
    if (energy < 0.00002f) {
        detectedMidiNote.store(-1, std::memory_order_relaxed);
        detectedFrequency.store(0.0f, std::memory_order_relaxed);
        return;
    }

    const int available = pitchWindowSize - maxPitchLag;
    for (int lag = minPitchLag; lag <= maxPitchLag; ++lag) {
        float sum = 0.0f;
        for (int i = 0; i < available; ++i) {
            const float a = pitchBuffer[(pitchWritePosition + i) % pitchWindowSize] - mean;
            const float b = pitchBuffer[(pitchWritePosition + i + lag) % pitchWindowSize] - mean;
            const float delta = a - b;
            sum += delta * delta;
        }
        difference[(size_t)lag] = sum;
    }

    float runningSum = 0.0f;
    for (int lag = minPitchLag; lag <= maxPitchLag; ++lag) {
        runningSum += difference[(size_t)lag];
        cumulativeMean[(size_t)lag] = difference[(size_t)lag] * (float)(lag - minPitchLag + 1)
                                     / (runningSum + 1.0e-12f);
    }

    int bestLag = 0;
    for (int lag = minPitchLag + 1; lag < maxPitchLag; ++lag) {
        if (cumulativeMean[(size_t)lag] < yinThreshold) {
            while (lag + 1 <= maxPitchLag &&
                   cumulativeMean[(size_t)(lag + 1)] < cumulativeMean[(size_t)lag])
                ++lag;
            bestLag = lag;
            break;
        }
    }

    if (bestLag == 0) {
        detectedMidiNote.store(-1, std::memory_order_relaxed);
        detectedFrequency.store(0.0f, std::memory_order_relaxed);
        return;
    }

    const float left = cumulativeMean[(size_t)(bestLag - 1)];
    const float centre = cumulativeMean[(size_t)bestLag];
    const float right = cumulativeMean[(size_t)(bestLag + 1)];
    const float denominator = left - 2.0f * centre + right;
    const float adjustment = std::abs(denominator) > 1.0e-9f
        ? 0.5f * (left - right) / denominator : 0.0f;
    const float refinedLag = (float)bestLag + juce::jlimit(-0.5f, 0.5f, adjustment);
    const float frequency = (float)(currentSampleRate / (double)(analysisDecimation * refinedLag));

    if (frequency < 65.0f || frequency > 1000.0f) {
        detectedMidiNote.store(-1, std::memory_order_relaxed);
        detectedFrequency.store(0.0f, std::memory_order_relaxed);
        return;
    }

    const float midi = 69.0f + 12.0f * std::log2(frequency / 440.0f);
    detectedFrequency.store(frequency, std::memory_order_relaxed);
    detectedMidiNote.store((int)std::lround(midi), std::memory_order_relaxed);
}

float TunePXOCobiAudioProcessor::readDelay(int channel, float delaySamples) const noexcept
{
    delaySamples = juce::jlimit(2.0f, (float)delayBufferSize - 2.0f, delaySamples);
    const float readPosition = (float)delayWritePosition - delaySamples;
    const float wrapped = readPosition < 0.0f ? readPosition + (float)delayBufferSize : readPosition;
    const int indexA = ((int)wrapped) % delayBufferSize;
    const float fraction = wrapped - std::floor(wrapped);
    const auto& line = delayBuffers[(size_t)channel];
    const int index0 = (indexA + delayBufferSize - 1) % delayBufferSize;
    const int index1 = indexA;
    const int index2 = (indexA + 1) % delayBufferSize;
    const int index3 = (indexA + 2) % delayBufferSize;
    const float y0 = line[(size_t)index0];
    const float y1 = line[(size_t)index1];
    const float y2 = line[(size_t)index2];
    const float y3 = line[(size_t)index3];

    // Cubic interpolation reduces the high-frequency loss and roughness
    // caused by linear fractional-delay reads during continuous pitch bends.
    const float c0 = y1;
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * fraction + c2) * fraction + c1) * fraction + c0;
}

float TunePXOCobiAudioProcessor::processPitchShift(int channel, float input, float ratio) noexcept
{
    auto& line = delayBuffers[(size_t)channel];
    line[(size_t)delayWritePosition] = input;

    constexpr float minDelay = 48.0f;
    ratio = juce::jlimit(0.5f, 2.0f, ratio);

    float phaseA = shifterPhase;
    float phaseB = phaseA + 0.5f;
    if (phaseB >= 1.0f) phaseB -= 1.0f;

    const float delayA = minDelay + phaseA * pitchShiftDelayRange;
    const float delayB = minDelay + phaseB * pitchShiftDelayRange;
    const float weightA = 0.5f - 0.5f * std::cos(twoPi * phaseA);
    const float weightB = 1.0f - weightA;
    const float shifted = readDelay(channel, delayA) * weightA + readDelay(channel, delayB) * weightB;

    // Fade gently to the dry path around unity pitch to avoid a hard switch click.
    const float correctionAmount = std::abs(1.0f - ratio);
    const float wetAmount = juce::jlimit(0.0f, 1.0f,
        (correctionAmount - 0.0005f) / 0.0025f);
    return input + (shifted - input) * wetAmount;
}

void TunePXOCobiAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int channels = juce::jmin(buffer.getNumChannels(), 2);
    const int numSamples = buffer.getNumSamples();
    if (channels <= 0 || numSamples <= 0)
        return;

    const int engine = (int)parameters.getRawParameterValue("ENGINE")->load();
    const int mode = (int)parameters.getRawParameterValue("MODE")->load();
    const int key = (int)parameters.getRawParameterValue("KEY")->load();
    const int scale = (int)parameters.getRawParameterValue("SCALE")->load();
    const float retune = parameters.getRawParameterValue("RETUNE")->load();
    const float humanize = parameters.getRawParameterValue("HUMANIZE")->load();
    const float mix = juce::jlimit(0.0f, 1.0f,
        parameters.getRawParameterValue("MIX")->load() * 0.01f);

    // Process detection and audio in the same sample loop. This keeps pitch
    // decisions and retune smoothing independent of the host's block size.
    const float speed = juce::jlimit(0.0f, 1.0f, retune * 0.01f);
    const float human = juce::jlimit(0.0f, 1.0f, humanize * 0.01f);
    const float modeTimeFactor = mode == 0 ? 1.0f : 0.12f;
    const float retuneSeconds = (0.0005f + speed * speed * 0.12f)
                              * modeTimeFactor * (1.0f + human * 2.0f);
    const float smoothing = retune <= 0.01f ? 1.0f
        : 1.0f - std::exp(-1.0f / (retuneSeconds * (float)currentSampleRate));

    auto* left = buffer.getWritePointer(0);
    auto* right = channels > 1 ? buffer.getWritePointer(1) : nullptr;

    for (int i = 0; i < numSamples; ++i)
    {
        const float mono = channels > 1 ? 0.5f * (left[i] + right[i]) : left[i];

        if ((audioSampleCounter++ % analysisDecimation) == 0)
        {
            pitchBuffer[(size_t)pitchWritePosition] = mono;
            pitchWritePosition = (pitchWritePosition + 1) % pitchWindowSize;
            pitchSamplesWritten = juce::jmin(pitchSamplesWritten + 1, pitchWindowSize);

            // Update at 128 decimated samples (~5.8 ms at 44.1 kHz),
            // using a complete rolling 512-sample analysis window.
            if (++samplesSinceAnalysis >= 128)
            {
                samplesSinceAnalysis = 0;
                if (pitchSamplesWritten >= pitchWindowSize)
                    analysePitch();
            }
        }

        const int detected = detectedMidiNote.load(std::memory_order_relaxed);
        const float frequency = detectedFrequency.load(std::memory_order_relaxed);
        float targetRatio = 1.0f;

        if (detected >= 0 && frequency > 0.0f)
        {
            const float originalMidi = 69.0f + 12.0f * std::log2(frequency / 440.0f);
            const float targetMidi = (float)quantiseMidiNote(originalMidi, key, scale);

            // Smooth the correction interval itself, not the target MIDI note.
            // This prevents the previous note's target from being subtracted
            // from the newly detected pitch during note transitions.
            const float desiredCorrectionSemitones = targetMidi - originalMidi;
            if (smoothedMidiNote < 0.0f)
                smoothedCorrectionSemitones = desiredCorrectionSemitones;
            else
                smoothedCorrectionSemitones +=
                    (desiredCorrectionSemitones - smoothedCorrectionSemitones) * smoothing;
            smoothedMidiNote = originalMidi;
            targetRatio = std::pow(2.0f, smoothedCorrectionSemitones / 12.0f);
            heldTargetPitchRatio = targetRatio;
            samplesSinceValidPitch = 0;
        }
        else
        {
            // Ignore very short unvoiced/detection gaps instead of abruptly
            // pulling the correction back to unity in the middle of a note.
            samplesSinceValidPitch += 1;
            const int holdSamples = (int)(0.045 * currentSampleRate);
            if (samplesSinceValidPitch <= holdSamples)
                targetRatio = heldTargetPitchRatio;
            else
                smoothedMidiNote = -1.0f;
        }

        // Smooth ratio changes, including brief detector dropouts, to suppress tiny RF-like ticks.
        constexpr float ratioSmoothingSeconds = 0.004f;
        const float ratioSmoothing = 1.0f - std::exp(
            -1.0f / (ratioSmoothingSeconds * (float)currentSampleRate));
        smoothedPitchRatio += (targetRatio - smoothedPitchRatio) * ratioSmoothing;
        const float ratio = smoothedPitchRatio;

        if (engine == 0) {
        const float dryLeft = left[i];
        const float wetLeft = processPitchShift(0, dryLeft, ratio);
        left[i] = dryLeft + (wetLeft - dryLeft) * mix;

        if (right != nullptr)
        {
            const float dryRight = right[i];
            const float wetRight = processPitchShift(1, dryRight, ratio);
            right[i] = dryRight + (wetRight - dryRight) * mix;
        }

        shifterPhase += (1.0f - ratio) / pitchShiftDelayRange;
        if (shifterPhase >= 1.0f) shifterPhase -= 1.0f;
        if (shifterPhase < 0.0f) shifterPhase += 1.0f;
        delayWritePosition = (delayWritePosition + 1) % delayBufferSize;
        }
    }
    if (engine == 1) {
        const int period = PsolaRealtimeAdapter::periodFromFrequency(currentSampleRate, (double)detectedFrequency.load(std::memory_order_relaxed));
        psola.process(buffer, period, (double)smoothedCorrectionSemitones);
    }
}

juce::AudioProcessorEditor* TunePXOCobiAudioProcessor::createEditor()
{
    return new TunePXOCobiAudioProcessorEditor(*this);
}

void TunePXOCobiAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void TunePXOCobiAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName(parameters.state.getType()))
        parameters.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TunePXOCobiAudioProcessor();
}
