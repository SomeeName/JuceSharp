// StemSeparator.cpp
//
// Two implementations behind one façade:
//
//   * DOTCHARTER_STEMS_DEMUCS_CPP=1 — full htdemucs inference via the
//     vendored sevagh/demucs.cpp tree. Audio I/O / resampling goes through
//     JUCE so we don't have to drag libnyquist in for WAV reads.
//
//   * otherwise — a "not supported" stub. Lets us ship the C ABI in the
//     default native build without forcing every user to download model
//     weights or compile the inference path. The managed wrapper detects
//     this case via dca_stems_is_supported() and falls back to the Python
//     demucs CLI service.
//
// The stub path keeps tests + CI green; the demucs.cpp path is opt-in.

#include "StemSeparator.h"
#include "DeviceRateWav.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#if DOTCHARTER_STEMS_DEMUCS_CPP
  // demucs.cpp public headers (added to the include path by CMake when
  // the FetchContent step succeeds). We deliberately keep the include
  // surface narrow so we don't pull <iostream> spam into the whole engine.
  #include <Eigen/Core>
  #include <Eigen/Dense>
  #include "model.hpp"
  #include "tensor.hpp"
  #include "dsp.hpp"
  // threaded_inference lives under demucs.cpp/cli-apps/ — we re-vendor a
  // copy in CMake so we can build it as part of juce_sharp.
  #include "threaded_inference.hpp"
#endif

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

namespace dotcharter::stems
{

#if DOTCHARTER_STEMS_DEMUCS_CPP

namespace
{
    // demucs.cpp requires 44.1 kHz stereo input. Anything else is converted
    // with juce::ResamplingAudioSource before inference — which PRE-FILTERS on
    // the downward direction, unlike the bare CatmullRom interpolator this used
    // to use; on output we write the stems back at the model's native rate.
    constexpr double kModelRate = 44100.0;

    juce::AudioBuffer<float> load_and_resample(const std::string& path)
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        auto file = juce::File(juce::String::fromUTF8(path.c_str()));
        std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
        if (!reader) return {};

        const auto srcRate     = reader->sampleRate;
        const auto srcChannels = (int) reader->numChannels;

        // REJECT, never truncate. `lengthInSamples` is a 64-bit count and this
        // was a bare `(int)` cast, so a file past 2^31 frames (~13.5 h at
        // 44.1 kHz) silently separated a WRAPPED, mostly-negative length — the
        // same D-03 class of bug dca_decode_audio_file already guards against.
        // juce::AudioBuffer indexes with int, so there is nothing to widen to;
        // refusing is the honest answer.
        if (reader->lengthInSamples <= 0
            || reader->lengthInSamples > (juce::int64) std::numeric_limits<int>::max())
            return {};
        const auto srcFrames = (int) reader->lengthInSamples;

        juce::AudioBuffer<float> srcBuf(srcChannels, srcFrames);
        reader->read(&srcBuf, 0, srcFrames, 0, true, srcChannels > 1);
        // ABI v43: a device-rate WAV's samples are worth v x its stored step.
        auto stored = dotcharter::devwav::probeFile(file);
        dotcharter::devwav::checkAgainstReader(stored, *reader);
        if (stored.decodeGain() != 1.0) srcBuf.applyGain((float) stored.decodeGain());

        // Down/up-mix to stereo first.
        juce::AudioBuffer<float> stereo(2, srcFrames);
        if (srcChannels == 1)
        {
            stereo.copyFrom(0, 0, srcBuf, 0, 0, srcFrames);
            stereo.copyFrom(1, 0, srcBuf, 0, 0, srcFrames);
        }
        else
        {
            stereo.copyFrom(0, 0, srcBuf, 0, 0, srcFrames);
            stereo.copyFrom(1, 0, srcBuf, 1 % srcChannels, 0, srcFrames);
        }

        if (std::abs(srcRate - kModelRate) < 0.5)
            return stereo;

        // ANTI-ALIAS ON THE WAY DOWN. This used a bare
        // juce::CatmullRomInterpolator, which has no pre-filter at all: a
        // 48 kHz (or 96 kHz) source going to the model's 44.1 kHz folded
        // everything above 22.05 kHz back into the audible band, and the model
        // then separated the aliases along with the music. Every source above
        // 44.1 kHz takes this path, which is most of them.
        //
        // juce::ResamplingAudioSource is JUCE's own converter and pre-filters
        // on the downward direction (its `ratio > 1` branch). Not
        // src/LoadResampler.h: dcload is UPSAMPLE-ONLY and deliberately refuses
        // exactly the direction this needs.
        const double ratio = srcRate / kModelRate;
        const int    outFrames = (int) std::ceil(srcFrames / ratio);
        if (outFrames <= 0) return {};

        juce::AudioBuffer<float> outBuf(2, outFrames);
        outBuf.clear();
        {
            constexpr int kChunk = 4096;
            juce::MemoryAudioSource mem(stereo, /*copyMemory*/ false, /*shouldLoop*/ false);
            juce::ResamplingAudioSource rs(&mem, /*deleteWhenRemoved*/ false, 2);
            rs.setResamplingRatio(ratio);
            rs.prepareToPlay(kChunk, kModelRate);
            for (int pos = 0; pos < outFrames; pos += kChunk)
            {
                const int n = juce::jmin(kChunk, outFrames - pos);
                juce::AudioBuffer<float> view(outBuf.getArrayOfWritePointers(), 2, pos, n);
                juce::AudioSourceChannelInfo info(&view, 0, n);
                rs.getNextAudioBlock(info);
            }
            rs.releaseResources();
        }
        return outBuf;
    }

    void write_wav(const std::string& path, const float* left, const float* right, int frames)
    {
        juce::WavAudioFormat wav;
        auto file = juce::File(juce::String::fromUTF8(path.c_str()));
        file.deleteFile();
        std::unique_ptr<juce::FileOutputStream> out(file.createOutputStream());
        if (!out) return;
        // p2-20: createWriterFor takes ownership of the raw stream pointer
        // *unconditionally* (it deletes it itself if writer construction
        // fails).  Release the unique_ptr BEFORE the call so a null-writer
        // return path doesn't double-free the stream.  Previously the
        // release() ran after the null-check, which on the failure branch
        // left both the writer-internal delete and ~unique_ptr<> racing for
        // the same FileOutputStream.
        auto* rawStream = out.release();
        std::unique_ptr<juce::AudioFormatWriter> writer(
            wav.createWriterFor(rawStream, kModelRate, 2, 32, {}, 0));
        if (!writer) return; // rawStream already freed by createWriterFor
        const float* chans[2] = { left, right };
        writer->writeFromFloatArrays(chans, 2, frames);
    }

    const char* target_name_for(int idx, bool is_4sources)
    {
        switch (idx)
        {
            case 0: return "drums";
            case 1: return "bass";
            case 2: return "other";
            case 3: return "vocals";
            case 4: return is_4sources ? nullptr : "guitar";
            case 5: return is_4sources ? nullptr : "piano";
            default: return nullptr;
        }
    }
} // namespace

bool is_supported() noexcept { return true; }

std::string describe_backend()
{
    return "demucs.cpp (htdemucs / hdemucs_mmi, native C++)";
}

int separate_file_cancellable(const std::string& src_path,
                              const std::string& out_dir,
                              const std::string& model_path,
                              int                num_threads,
                              const CancellableProgressCallback& progress)
{
    // `progress` returning FALSE means cancel. A null callback is "no progress
    // wanted", which is also "never cancelled" — so every report site below is
    // `if (progress && !progress(...)) return 5;` and a host that passes
    // nothing gets the pre-v36 behaviour exactly.
    if (src_path.empty() || out_dir.empty() || model_path.empty()) return 1;
    if (!std::filesystem::exists(model_path))                       return 2;
    std::filesystem::create_directories(out_dir);

    if (progress && !progress(0.02f, "loading model"))              return 5;
    demucscpp::demucs_model model{};
    if (!demucscpp::load_demucs_model(model_path, &model))          return 3;

    if (progress && !progress(0.10f, "decoding audio"))             return 5;
    auto buf = load_and_resample(src_path);
    if (buf.getNumSamples() <= 0)                                   return 4;

    const int frames = buf.getNumSamples();
    Eigen::MatrixXf audio(2, frames);
    for (int i = 0; i < frames; ++i)
    {
        audio(0, i) = buf.getReadPointer(0)[i];
        audio(1, i) = buf.getReadPointer(1)[i];
    }

    // The LAST cheap place to stop: everything before this is file I/O and
    // everything after it is the model run.
    if (progress && !progress(0.20f, "running inference"))          return 5;
    const int threads = num_threads > 0
        ? num_threads
        : std::max(1, (int) std::thread::hardware_concurrency());

    // demucs.cpp's threaded_inference takes no progress callback in its public
    // signature — only an optional log prefix. We bracket the call with our
    // own progress hooks (0.20 before / 0.95 after) instead.
    Eigen::Tensor3dXf out_targets = demucscppthreaded::threaded_inference(
        model, audio, threads, std::string{});
    // A cancel that arrived DURING inference lands here: demucs.cpp's threaded
    // call takes no callback, so it cannot be interrupted part-way. Stopping
    // here still skips every WAV write, which is the expensive remainder.
    if (progress && !progress(0.95f, "inference complete"))          return 5;

    const bool is4 = model.is_4sources;
    const int  nb  = is4 ? 4 : 6;
    // The htdemucs model's output time dimension can differ from the input
    // frame count due to STFT windowing / convolutional edge effects.
    // Clamp to the actual tensor extent to avoid reading past its end.
    const int outFrames = std::min(frames, (int) out_targets.dimension(2));
    for (int t = 0; t < nb; ++t)
    {
        const char* name = target_name_for(t, is4);
        if (!name) continue;
        const auto path = (std::filesystem::path(out_dir) / (std::string(name) + ".wav")).string();
        std::vector<float> L(outFrames), R(outFrames);
        for (int i = 0; i < outFrames; ++i) { L[i] = out_targets(t, 0, i); R[i] = out_targets(t, 1, i); }
        write_wav(path, L.data(), R.data(), outFrames);
        // THE CHECKED POINT BETWEEN THE PER-TARGET WRITES (W4-N19). Four or six
        // files, pure I/O after the model has already run, and it used to be
        // uninterruptible. A cancel here leaves the stems written so far on
        // disk: the caller asked to stop, and deleting a partial set is a
        // decision for whoever owns out_dir, not for this function.
        if (progress && !progress(0.95f + (0.05f * (t + 1) / nb), "writing stems"))
            return 5;
    }
    if (progress) progress(1.0f, "done");   // the result is complete; too late to cancel
    return 0;
}

// ONE IMPLEMENTATION: the v35 entry point is this and a callback that never
// cancels, so the two can never drift.
int separate_file(const std::string& src_path,
                  const std::string& out_dir,
                  const std::string& model_path,
                  int                num_threads,
                  const ProgressCallback& progress)
{
    if (!progress)
        return separate_file_cancellable(src_path, out_dir, model_path, num_threads, nullptr);

    return separate_file_cancellable(
        src_path, out_dir, model_path, num_threads,
        [&progress](float frac, const char* stage) { progress(frac, stage); return true; });
}

#else // !DOTCHARTER_STEMS_DEMUCS_CPP

bool is_supported() noexcept { return false; }

std::string describe_backend()
{
    return "unsupported (rebuild juce_sharp with -DDOTCHARTER_STEMS_DEMUCS_CPP=ON)";
}

int separate_file(const std::string&, const std::string&, const std::string&,
                  int, const ProgressCallback&)
{
    return -1;
}

int separate_file_cancellable(const std::string&, const std::string&, const std::string&,
                              int, const CancellableProgressCallback&)
{
    return -1;
}

#endif

} // namespace dotcharter::stems
