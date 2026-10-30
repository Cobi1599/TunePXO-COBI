#include "sola.h"

#include <cmath>
#include <cassert>

#include <juce_audio_basics/juce_audio_basics.h>

namespace TD_pitch_shift {
namespace {

template<int length>
auto GetHanningWindow() {
    std::array<float, length> arr{};
    for (int i = 0; i < length; i++) {
        arr[i] = static_cast<float>(
            0.5 * (1.0 - std::cos(2.0 * juce::MathConstants<double>::pi * i / (length - 1)))
        );
    }
    return arr;
}
constexpr static int kWindowLen = 2048;
const static auto kHanningWindow = GetHanningWindow<kWindowLen>();

} // namespace 

Sola::Sola(int numChannels, int frameSize, int maxPeriod) :
    mNumChannels(numChannels),
    mFrameSize(frameSize),
    mMaxPeriod(maxPeriod),
    mAnalysis{0, frameSize + static_cast<int>(kMaxStretchRatio+1.0) * maxPeriod, numChannels, static_cast<int>(kMaxStretchRatio) * maxPeriod, maxPeriod},
    mSynthesis{0, frameSize + static_cast<int>(kMaxStretchRatio) * maxPeriod, numChannels, static_cast<int>(kMaxStretchRatio) * maxPeriod, maxPeriod},
    mSearchRange{maxPeriod + frameSize, maxPeriod},
    mReadyOutRange{static_cast<int>(kMaxStretchRatio) * maxPeriod + frameSize, static_cast<int>(kMaxStretchRatio) * maxPeriod},
    mPrevFrameEndOffset(frameSize),
    mWindowFunction(2 * maxPeriod),
    mCurrentWindowSize(0),
    mLinearInterpolator{} {
    assert(mNumChannels > 0 && mFrameSize > 0 && mMaxPeriod > 0);

    mAnalysis.buffer.setSize(mNumChannels, mAnalysis.numSamples);
    mAnalysis.buffer.clear();
    mSynthesis.buffer.setSize(mNumChannels, mSynthesis.numSamples);
    mSynthesis.buffer.clear();
};

inline int Sola::getFrameSize() const noexcept{
    return mFrameSize;
}

void Sola::Process(juce::AudioBuffer<float>& frame, int currentFramePeriod, double semitones) {
    assert(frame.getNumChannels() == mNumChannels);
    assert(frame.getNumSamples() == mFrameSize);
    assert(-12.0 <= semitones && semitones <= 12.0);
    assert(0 < currentFramePeriod && currentFramePeriod <= mMaxPeriod);

    // Push input samples to analysis circular buffer 
    for (int c = 0; c < mNumChannels; ++c) {
        const int samplesUntilWrap = mAnalysis.numSamples - mAnalysis.head;
        if (samplesUntilWrap >= mFrameSize) { 
            mAnalysis.buffer.copyFrom(c, mAnalysis.head, frame.getReadPointer(c), mFrameSize);
        } else { 
            const int partOneSize = samplesUntilWrap;
            const int partTwoSize = mFrameSize - partOneSize;
            mAnalysis.buffer.copyFrom(c, mAnalysis.head, frame.getReadPointer(c), partOneSize);
            mAnalysis.buffer.copyFrom(c, 0, frame.getReadPointer(c, partOneSize), partTwoSize);
        }
    }
    // Advance head of buffers since new inputs are pushed
    mAnalysis.head = (mAnalysis.head + mFrameSize) % mAnalysis.numSamples;
    mSynthesis.head = (mSynthesis.head + mFrameSize) % mSynthesis.numSamples;
    // Offset is relative distance from head.
    // (Offset abstracts away the circular wrap-around so that one can consider indexing linear conceptually.)
    mAnalysis.lastMarkOffset += mFrameSize;
    mSynthesis.lastMarkOffset += mFrameSize;

    int analysisPeriod = mAnalysis.lastPeriod; // analysis pitch mark stride
    int synthesisPeriod = mSynthesis.lastPeriod; // synthesis pitch mark stride

    assert(mAnalysis.lastMarkOffset > mSearchRange.startOffset);
    assert(mSynthesis.lastMarkOffset > mSearchRange.startOffset);
    int nextAnalysisOffset = mAnalysis.lastMarkOffset - mAnalysis.lastPeriod; // next analysis pitch mark offset
    int nextSynthesisOffset = mSynthesis.lastMarkOffset - mSynthesis.lastPeriod; // next synthesis pitch mark offset

    while(nextAnalysisOffset > mSearchRange.endOffset || nextSynthesisOffset > mSearchRange.endOffset) {
        assert(mSearchRange.startOffset >= nextAnalysisOffset && mSearchRange.startOffset >= nextSynthesisOffset);

        // Analysis step
        if(nextAnalysisOffset >= nextSynthesisOffset) { 
            mAnalysis.lastMarkOffset = nextAnalysisOffset;
            mAnalysis.lastPeriod = analysisPeriod; 

            if(mPrevFrameEndOffset >= nextAnalysisOffset) 
                analysisPeriod = currentFramePeriod;

            nextAnalysisOffset -= analysisPeriod;
        } 
        // Synthesis step
        else { 
            const int grainSize = 2 * mAnalysis.lastPeriod;
            const int grainSizeHalf = mAnalysis.lastPeriod;
            
            // Update window function to match new segment size 
            if(mCurrentWindowSize != grainSize) {
                mLinearInterpolator.reset();
                
                mCurrentWindowSize = grainSize;
                const double stretchRatio = kWindowLen / static_cast<double>(mCurrentWindowSize);
                mLinearInterpolator.process(stretchRatio, kHanningWindow.data(), mWindowFunction.data(), mCurrentWindowSize);
            }
            
            const int analysisStart = mAnalysis.OffsetToIdx(mAnalysis.lastMarkOffset + grainSizeHalf); // Index where segment copy starts
            assert(mReadyOutRange.startOffset >= nextSynthesisOffset + grainSizeHalf);
            assert(nextSynthesisOffset - grainSizeHalf > 0);
            const int synthesisStart = mSynthesis.OffsetToIdx(nextSynthesisOffset + grainSizeHalf); // Index where segment paste starts
            
            // Overlap and Add 
            for(int c = 0; c < mNumChannels; ++c) {
                int remainingSamples = grainSize;
                int windowOffset = 0;
                int synthesisIdx = synthesisStart;
                int analysisIdx = analysisStart;
                
                while (remainingSamples > 0) {
                    const int synthesisSamplesUntilWrap = mSynthesis.numSamples - synthesisIdx;
                    const int analysisSamplesUntilWrap = mAnalysis.numSamples - analysisIdx;
                    const int samplesToProcess = std::min({remainingSamples, synthesisSamplesUntilWrap, analysisSamplesUntilWrap});
                    
                    float* resultPtr = mSynthesis.buffer.getWritePointer(c, synthesisIdx);
                    const float* windowPtr = mWindowFunction.data() + windowOffset;
                    const float* segmentPtr = mAnalysis.buffer.getReadPointer(c, analysisIdx);
                    
                    // apply window function to segment and store it in synthesis circular buffer
                    juce::FloatVectorOperations::addWithMultiply(resultPtr, windowPtr, segmentPtr, samplesToProcess); 
                    
                    remainingSamples -= samplesToProcess;
                    windowOffset += samplesToProcess;
                    synthesisIdx = (synthesisIdx + samplesToProcess) % mSynthesis.numSamples;
                    analysisIdx = (analysisIdx + samplesToProcess) % mAnalysis.numSamples;
                }
            }
            
            mSynthesis.lastMarkOffset = nextSynthesisOffset;

            if(mPrevFrameEndOffset >= nextSynthesisOffset) {
                const double stretchRatio = std::pow(2.0, -semitones / 12.0);
                synthesisPeriod = static_cast<int>(std::lround(currentFramePeriod * stretchRatio));
            }
            nextSynthesisOffset -= synthesisPeriod;
        }
    }
    mAnalysis.lastPeriod = analysisPeriod;
    mSynthesis.lastPeriod = synthesisPeriod;
    assert(2 * mMaxPeriod >= mAnalysis.lastMarkOffset && mAnalysis.lastMarkOffset > mMaxPeriod);
    assert(2 * mMaxPeriod + mMaxPeriod >= mSynthesis.lastMarkOffset&& mSynthesis.lastMarkOffset > mMaxPeriod);
    
    // Copy output from synthesis buffer to frame
    const int readStartIdx = mSynthesis.OffsetToIdx(mReadyOutRange.startOffset);
    for (int c = 0; c < mNumChannels; ++c) {
        const int samplesUntilWrap = mSynthesis.numSamples - readStartIdx;
        if (samplesUntilWrap >= mFrameSize) {
            frame.copyFrom(c, 0, mSynthesis.buffer.getReadPointer(c, readStartIdx), mFrameSize);
            mSynthesis.buffer.clear(c, readStartIdx, mFrameSize);
        } 
        else {
            const int partOneSize = samplesUntilWrap;
            const int partTwoSize = mFrameSize - partOneSize;
            frame.copyFrom(c, 0, mSynthesis.buffer.getReadPointer(c, readStartIdx), partOneSize);
            frame.copyFrom(c, partOneSize, mSynthesis.buffer.getReadPointer(c, 0), partTwoSize);
            mSynthesis.buffer.clear(c, readStartIdx, partOneSize);
            mSynthesis.buffer.clear(c, 0, partTwoSize);
        }
    }
}

} // namespace TD_pitch_shift