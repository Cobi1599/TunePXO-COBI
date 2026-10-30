#pragma once

#include <array>
#include <cassert>
#include <cmath>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

namespace TD_pitch_shift {

class Sola {
public:
    constexpr static double kMaxStretchRatio = 2.0;
    constexpr static double kMinStretchRatio = 0.5;
    
    Sola(int numChannels, int frameSize, int maxPeriod);
    Sola(const Sola& other) = delete;
    Sola& operator=(const Sola&) = delete;
    Sola(Sola&& other) = default;
    Sola& operator=(const Sola&&) = delete;
    
    inline int getFrameSize() const noexcept;
    void Process(juce::AudioBuffer<float>& frame, int currentFramePeriod, double semitones);
private:
    struct CircularBuffer {
        CircularBuffer(int head, int numSamples, int numChannels, int lastMarkOffset, int lastPeriod) : 
            head(head), numSamples(numSamples), buffer(numChannels, numSamples), 
            lastMarkOffset(lastMarkOffset), lastPeriod(lastPeriod) {};
        int head; // The index of the most recent sample in the buffer
        const int numSamples;
        juce::AudioBuffer<float> buffer;

        int lastMarkOffset;
        int lastPeriod;
        
        // Converts "offset" to index value
        // Offset is relative distance from head.
        // (Offset abstracts away the circular wrap-around so that one can consider indexing linear conceptually.)
        int OffsetToIdx(int offset) const noexcept{
            assert(0 <= offset && offset <= numSamples);
            const int rawIdx = head - offset;
            return rawIdx >= 0 ? rawIdx : rawIdx + numSamples;
        }
    };
    CircularBuffer mAnalysis, mSynthesis;

    struct Region {
        const int startOffset;
        const int endOffset;
    };
    const Region mSearchRange; 
    const Region mReadyOutRange; // samples in this region are complete at the end of the process function.
    const int mPrevFrameEndOffset;

    std::vector<float> mWindowFunction;
    int mCurrentWindowSize;

    juce::Interpolators::Linear mLinearInterpolator;
    
    const int mFrameSize; 
    const int mNumChannels;
    const int mMaxPeriod;
};

} // namespace TD_pitch_shift
