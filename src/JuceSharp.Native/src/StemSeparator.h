// StemSeparator.h
//
// Native AI stem-separation backend exposed through the dca_stems_* C ABI.
//
// When CMake is configured with DOTCHARTER_STEMS_DEMUCS_CPP=ON, this wraps
// sevagh/demucs.cpp (htdemucs / hdemucs_mmi) with JUCE-based WAV I/O and
// resampling. When the option is OFF, the implementation compiles to a
// pure stub that reports "not supported" and the managed side gracefully
// falls back to the Python demucs CLI.

#pragma once

#include <string>
#include <functional>

namespace dotcharter::stems
{
    using ProgressCallback = std::function<void(float fraction, const char* stage)>;

    /// The SAME callback, plus a cancellation channel: return false to stop
    /// (ABI v36, W4-N19). A separation is a multi-minute call and the only way a
    /// host could stop one was to abandon the thread and let it finish writing
    /// stems nobody wanted — which is also why the managed side could honour
    /// "a cancelled separation throws OperationCanceledException" only by
    /// walking away from work that was still running.
    ///
    /// A SECOND typedef rather than a changed one, on both sides of the ABI:
    /// `dca_stems_separate_file` and its `dca_stems_progress_cb` keep their
    /// exact v35 shape, so every existing binding, and the three managed tests
    /// that invoke the delegate directly, are untouched.
    using CancellableProgressCallback = std::function<bool(float fraction, const char* stage)>;

    // Compile-time capability check. Mirrors `dca_stems_is_supported`.
    bool is_supported() noexcept;

    // Human-readable backend identifier (e.g. "demucs.cpp htdemucs 4s" or
    // "unsupported (DOTCHARTER_STEMS_DEMUCS_CPP=OFF)"). UTF-8.
    std::string describe_backend();

    // Decodes `src_path`, runs the model whose weights live at `model_path`,
    // and writes per-source 32-bit-float WAVs under `out_dir`. Returns 0 on
    // success or a non-zero error code (which dotcharter_audio.cpp maps to
    // the appropriate dca_status). `num_threads <= 0` => hardware concurrency.
    //
    // Error codes: 1 empty path argument, 2 model file missing, 3 weights would
    // not load, 4 audio would not decode, 5 CANCELLED by the callback (v36),
    // -1 the backend is not compiled in.
    //
    // ONE IMPLEMENTATION. This overload forwards to the cancellable one with a
    // callback that always answers "keep going", so the two can never drift —
    // the same rule that collapsed the four copied seat-0 capture exports.
    int separate_file(const std::string& src_path,
                      const std::string& out_dir,
                      const std::string& model_path,
                      int                num_threads,
                      const ProgressCallback& progress);

    // The cancellable form (ABI v36). Identical work; `progress` returning
    // false stops it at the next checked point and the call answers 5.
    //
    // The checked points are between the coarse phases AND between the
    // per-target WAV writes, which is the one that matters in practice: the
    // 0.95 -> 1.0 stretch writes four or six files and used to be
    // uninterruptible even though it is pure I/O after the model has run.
    // Inference itself is demucs.cpp's own threaded call and takes no callback,
    // so it CANNOT be interrupted part-way — a cancel arriving during it is
    // honoured the moment it returns. That is a real bound on how fast a cancel
    // lands, and it is documented rather than pretended away.
    int separate_file_cancellable(const std::string& src_path,
                                  const std::string& out_dir,
                                  const std::string& model_path,
                                  int                num_threads,
                                  const CancellableProgressCallback& progress);
} // namespace dotcharter::stems
