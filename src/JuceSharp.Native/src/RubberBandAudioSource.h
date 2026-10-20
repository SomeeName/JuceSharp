#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace RubberBand { class RubberBandStretcher; }

namespace dotcharter {

/// JUCE PositionableAudioSource adapter around RubberBand::RubberBandStretcher.
/// Pulls from an inner PositionableAudioSource and stretches/shifts on the fly.
///
/// Conventions:
///   * timeRatio = output_duration / input_duration. To play 2x faster, pass 0.5.
///   * pitchScale = output_frequency / input_frequency. Use pow(2, semitones/12).
///   * baseRatio is the SR-conversion ratio (fileSr / devSr). Effective time
///     ratio sent to rubberband is baseRatio * userTempoRatio.
class RubberBandAudioSource : public juce::PositionableAudioSource
{
public:
    /// `engineQuality` points at the OWNING ENGINE's quality flag (0 = Finer,
    /// 1 = Faster), read at prepareToPlay when the stretcher is constructed.
    /// It used to be a process-global, so `dca_engine_set_stretch_quality`
    /// ignored its handle and re-voiced every OTHER engine in the process
    /// along with the one the caller named. Null means Finer.
    RubberBandAudioSource(juce::PositionableAudioSource* inner,
                          bool                           deleteInnerWhenRemoved,
                          int                            numChannels,
                          const std::atomic<int>*        engineQuality = nullptr);
    ~RubberBandAudioSource() override;

    void prepareToPlay(int blockSize, double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override;

    void setNextReadPosition(juce::int64 newPosition) override;
    juce::int64 getNextReadPosition()    const override;
    juce::int64 getTotalLength()         const override;
    bool        isLooping()              const override;
    void        setLooping(bool shouldLoop) override;

    /// Read-ahead samples currently buffered inside Rubber Band, in output
    /// (device-rate) samples. 0 when the fast-bypass path is active or before
    /// any audio has been pulled. Used by the host to compensate the audible
    /// playback position so the playhead doesn't drift when tempo != 1.
    int getReadAheadOutputSamples() const noexcept;

    /// Which Rubber Band engine the stretcher was actually built with at the
    /// last prepareToPlay: true = R2 (Faster), false = R3 (Finer). Test hook —
    /// the choice is otherwise invisible from outside, which is how a
    /// process-global quality flag went unnoticed.
    bool preparedWithFasterEngineForTest() const noexcept
    { return preparedFaster_; }

    /// Most recently applied user tempo factor (1.0 = normal speed).
    float currentTempoFactor() const noexcept { return tempoFactor_.load(std::memory_order_relaxed); }

    /// Device sample rate used at last prepareToPlay; 0 if not prepared.
    double currentDeviceSampleRate() const noexcept { return deviceSampleRate_; }

    /// Source-rate ratio (fileSr/devSr). Multiplied into the effective time ratio.
    void setBaseRatio    (double r) noexcept;
    /// User tempo factor: 1.0 = normal, 2.0 = twice as fast. Maps to 1/r time ratio.
    void setTempoFactor  (float  f) noexcept;
    /// Pitch shift in semitones; rubberband uses pitch scale = 2^(st/12).
    void setPitchSemitones(float st) noexcept;

    /// Force pass-through mode (skips Rubber Band entirely, just calls
    /// inner->getNextAudioBlock). Used when this source has been re-routed
    /// into a stem group whose shared stretcher handles tempo/pitch for
    /// the whole group — we still need the per-source chain for volume /
    /// gain but must NOT apply per-source time stretching.
    /// CONTROL THREAD: it also resets the stretcher (see repositionInner) —
    /// whatever it held stops being the song's continuation once the inner
    /// plays on without it.
    void setBypass(bool bypassed);

    /// CONTROL THREAD. Moves the inner through `move` (a transport seek or
    /// stop) and resets the stretcher in ONE step under stretcherMutex_, so
    /// no audio block can run between the two: one that did would either keep
    /// pre-seek audio in the stretcher or drop post-seek audio from it.
    /// Before JuceSharp #32 the transport seek never reached the stretcher at
    /// all, so a seek while slowed played out the pre-seek read-ahead.
    /// Not on the audio thread, deliberately: Rubber Band's R3 reset() is NOT
    /// real-time safe (BinClassifier::reset frees and reallocates — the RT
    /// gate caught it). The callback waits on this lock for one transport
    /// move plus one reset per seek, which is what a seek always cost here.
    template <typename Move>
    void repositionInner(Move&& move)
    {
        std::lock_guard<std::mutex> lk(stretcherMutex_);
        move();
        resetStretcherLocked();
    }
    /// repositionInner with nothing to move: a stem-group stretcher whose
    /// stems were seeked one by one.
    void resetStretcher() { repositionInner([] {}); }

    /// Global toggle for the stretcher engine. False (default) selects
    /// Rubber Band R3 (Finer); true selects R2 (Faster). Affects sources
    /// constructed *after* the call — existing instances keep their
    /// current engine until prepareToPlay is invoked again. The host
    /// flips this when stems are loaded so 5+ concurrent stretchers fit
    /// inside the audio thread's CPU budget. It is PER ENGINE: see the ctor.

private:
    /// The owning engine's quality flag; see the constructor.
    const std::atomic<int>*                       engineQuality_ = nullptr;
    bool                                          preparedFaster_ = false;

    void applyRatiosLocked();
    /// The stretch path of getNextAudioBlock. Caller must hold stretcherMutex_.
    void renderStretchedLocked(const juce::AudioSourceChannelInfo& info);
    /// Start stretching a CLEAN stretcher from the inner's current position:
    /// pad its input with the silence Rubber Band asks for and arrange to drop
    /// the matching start delay. Caller holds stretcherMutex_.
    void beginStretchLocked();
    /// Back to a clean stretcher (nothing pushed since). Caller holds the
    /// lock. NEVER on the audio thread — see repositionInner.
    void resetStretcherLocked();
    int  pullBlockSize() const noexcept;

    /// True once the stretcher has consumed the inner since its last reset.
    /// The inner is then read past what the listener has heard and the
    /// stretcher's read-ahead IS the song's next audio, so every block goes
    /// through it — at unity too — until the next reset (JuceSharp #32).
    /// False: reset or freshly built, nothing pushed; the unity fast path may
    /// run. Touched only under stretcherMutex_.
    bool                                          stretching_ = false;
    /// Output samples still to drop after a padded start (getStartDelay()).
    int                                           discardRemaining_ = 0;

    juce::PositionableAudioSource*                inner_;
    bool                                          deleteInner_;
    int                                           numChannels_ = 2;
    double                                        deviceSampleRate_ = 0.0;
    int                                           deviceBlock_ = 0;

    /// Guards stretcher_'s LIFETIME. prepareToPlay constructs it and
    /// releaseResources resets it (both on the device thread, at every device
    /// change), while the audio thread is inside getNextAudioBlock using it.
    /// Uncontended in steady state — the only contenders are device (re)start,
    /// a seek or stop (repositionInner: one transport move plus one reset) and
    /// a stem-group bypass change, and the 250 Hz position poll deliberately
    /// does NOT take it (see readAheadOut_), so it can never stall the audio
    /// thread for longer than one of those.
    mutable std::mutex                            stretcherMutex_;
    std::unique_ptr<RubberBand::RubberBandStretcher> stretcher_;

    /// Read-ahead buffered inside Rubber Band, in output samples, PUBLISHED by
    /// the audio thread once per block. getReadAheadOutputSamples() reads this
    /// instead of calling stretcher_->available() from the polling thread —
    /// that call used to dereference the unique_ptr with nothing stopping
    /// releaseResources() from resetting it mid-call.
    std::atomic<int>                              readAheadOut_ { 0 };

    juce::AudioBuffer<float>                      pullBuffer_;
    juce::AudioBuffer<float>                      retrieveBuffer_;
    std::vector<float*>                           pullPtrs_;
    std::vector<const float*>                     pushPtrs_;
    std::vector<float*>                           retrievePtrs_;

    std::atomic<double>                           baseRatio_   { 1.0 };
    std::atomic<float>                            tempoFactor_ { 1.0f };
    std::atomic<float>                            pitchScale_  { 1.0f };
    std::atomic<bool>                             dirty_       { true };
    std::atomic<bool>                             bypass_      { false };
};

} // namespace dotcharter
