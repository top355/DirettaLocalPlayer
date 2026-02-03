#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include <string>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <thread>
#include "SimdUtils.h"
#include "DirettaSync.h" // [NEW] Using DirettaSync

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>
}

/**
 * @brief DSD source format (DSF vs DFF)
 */

/**
 * @brief Audio track information
 */
struct TrackInfo {
    // ⭐ v1.2.0 : DSD source format enum
    enum class DSDSourceFormat {
        DSF,      // LSB first
        DFF,      // MSB first  
        Unknown
    };
    
    // Core Identity
    std::string uri;         // Unique Resource Identifier (previously 'path')
    std::string path;        // Alias for compatibility/clarity
    std::string metadata;    // Raw metadata string
    
    // Audio Format
    uint32_t sampleRate = 0;
    uint32_t bitDepth = 0;
    uint32_t channels = 2;
    std::string codec;
    uint32_t bitsPerSample = 0; // Container bits
    
    enum class S24Alignment {
        Unknown,
        Lsb,  // Standard S24_LE (data in bytes 0-2)
        Msb   // Left-justified (data in bytes 1-3)
    };
    S24Alignment s24Alignment = S24Alignment::Unknown;
    uint64_t duration = 0;   // In frames/ticks?
    double durationSeconds = 0.0; // Calculated duration
    
    bool isDSD = false;
    int dsdRate = 0;
    bool isCompressed = true;
    
    // ⭐ v1.2.0 : Format source DSD
    DSDSourceFormat dsdSourceFormat = DSDSourceFormat::Unknown;

    // Virtual Track Support (CUE / Segment Playback)
    double startTime = 0.0;          // Offset in seconds
    double virtualDuration = -1.0;   // Duration of this segment (-1 = end)
    
    // Metadata
    std::string trackTitle;
    std::string trackArtist;

    TrackInfo() = default;
};
/**
 * @brief Audio buffer for streaming
 */
class AudioBuffer {
public:
    AudioBuffer(size_t size = 0);
    ~AudioBuffer();

    // ⭐ Rule of Three: Prevent copying (would cause double-delete)
    AudioBuffer(const AudioBuffer&) = delete;
    AudioBuffer& operator=(const AudioBuffer&) = delete;

    // Allow moving (safe)
    AudioBuffer(AudioBuffer&& other) noexcept;
    AudioBuffer& operator=(AudioBuffer&& other) noexcept;

    /**
     * @brief Set logical size, growing capacity if needed
     * 
     * If size <= capacity, no allocation occurs (grow-only).
     * If size > capacity, buffer is reallocated with 1.5x growth.
     */
    void resize(size_t size);

    /**
     * @brief Pre-allocate capacity without changing logical size
     * 
     * Use this to reserve space before a series of operations.
     */
    void ensureCapacity(size_t cap);

    size_t size() const { return m_size; }
    size_t capacity() const { return m_capacity; }
    uint8_t* data() { return m_data; }
    const uint8_t* data() const { return m_data; }

private:
    static constexpr size_t ALIGNMENT = 64;  // AVX-512 / cache line

    void growCapacity(size_t needed);

    uint8_t* m_data;
    size_t m_size;
    size_t m_capacity;
};

/**
 * @brief Audio decoder for a single track
 */
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();
    
    /**
     * @brief Open and decode a URL
     * @param url Audio file URL
     * @return true if successful, false otherwise
     */
    bool open(const std::string& url);
    
    /**
     * @brief Close the decoder
     */
    void close();
    
    /**
     * @brief Get track information
     * @return Track info
     */
    const TrackInfo& getTrackInfo() const { return m_trackInfo; }
    
    /**
     * @brief Read and decode audio samples
     * @param buffer Output buffer
     * @param numSamples Number of samples to read
     * @param outputRate Target sample rate
     * @param outputBits Target bit depth
     * @return Number of samples actually read (0 = EOF)
     */
    size_t readSamples(AudioBuffer& buffer, size_t numSamples, 
                      uint32_t outputRate, uint32_t outputBits);
    
    /**
     * @brief Check if EOF reached
     * @return true if at end of file
     */
    bool isEOF() const { return m_eof; }
    
    /**
     * @brief Seek to a specific position in the audio file
     * @param seconds Position in seconds
     * @return true if successful, false otherwise
     */
    bool seek(double seconds);
    
private:
    AVFormatContext* m_formatContext;
    AVCodecContext* m_codecContext;
    SwrContext* m_swrContext;
    int m_audioStreamIndex;
    TrackInfo m_trackInfo;
    bool m_eof;
    
    // ⭐ DSD Native Mode
    bool m_rawDSD;           // True if reading raw DSD packets (no decoding)
    AVPacket* m_packet;      // For raw packet reading
    
    // ════════════════════════════════════════════════════════════════
    // ⭐ v1.3.2: PCM FIFO and Bypass Optimization
    // ════════════════════════════════════════════════════════════════
    AVAudioFifo* m_pcmFifo = nullptr;   // O(1) circular buffer for PCM
    bool m_bypassMode = false;          // True if bit-perfect path (no resampling)
    bool m_resamplerInitialized = false; // Flag to track initialization
    
    // v1.3.2: Staging buffers for DSD (aligned for SIMD)
    alignas(64) uint8_t m_dsdPoolL[131072]; // 128KB
    alignas(64) uint8_t m_dsdPoolR[131072]; // 128KB
    
    // DSD Remainder handling
    size_t m_dsdRemainderCount = 0;
    AudioBuffer m_dsdRemainderBuffer;
    
    // Staging buffers for DSD
    AudioBuffer m_dsdLeftBuffer;
    AudioBuffer m_dsdRightBuffer;
    size_t m_dsdBufferCapacity = 0;

    // ✅ 工作 Buffer 池，扩容至 2MB 以支持 DSD1024 和 1536kHz PCM
    alignas(64) uint8_t m_alignedPool[2097152]; // 2MB
    AudioBuffer m_workBuffer;
    
    // DSD 余量缓冲 - 仅存储单次数据包处理的剩余字节 (< 1个数据包)
    // 主缓冲逻辑已移至 DirettaRingBuffer
    
    // Debug counters (per-instance to avoid race conditions)
    int m_readCallCount = 0;
    int m_packetCount = 0;
    bool m_dsdWarningShown = false;
    bool m_interleavingLoggedDOP = false;
    bool m_interleavingLoggedNative = false;
    bool m_dumpedFirstPacket = false;
    bool m_bitReversalLogged = false;
    bool m_resamplingLogged = false;
    bool m_resamplerInitLogged = false;
    
    bool initResampler(uint32_t outputRate, uint32_t outputBits);
    bool canBypass(uint32_t outputRate, uint32_t outputBits) const;
};

// ═══════════════════════════════════════════════════════════════
// Forward declaration for AudioFormat (needed by NextTrackCallback)
// ═══════════════════════════════════════════════════════════════
struct AudioFormat;

/**
 * @brief Audio Engine with gapless playback support
 * 
 * Manages audio decoding, buffering, and gapless transitions.
 * Supports pre-loading next track for seamless playback.
 */
class AudioEngine {
public:
    /**
     * @brief Playback state
     */
    enum class State {
        STOPPED,
        PLAYING,
        PAUSED,
        TRANSITIONING
    };
    
    /**
     * @brief Callback for audio data ready
     * @param buffer Audio buffer
     * @param samples Number of samples
     * @param sampleRate Sample rate
     * @param bitDepth Bit depth
     * @param channels Number of channels
     * @return true to continue, false to stop
     */
    using AudioCallback = std::function<bool(const AudioBuffer&, size_t, 
                                            uint32_t, uint32_t, uint32_t)>;
    
    /**
     * @brief Callback for track change
     * @param trackNumber New track number
     * @param trackInfo Track information
     * @param uri Track URI
     * @param metadata Track metadata
     */
    using TrackChangeCallback = std::function<void(int, const TrackInfo&, 
                                                   const std::string&, const std::string&)>;

    /**
     * @brief Callback for track end
     */
    using TrackEndCallback = std::function<void()>;
    
    // ═══════════════════════════════════════════════════════════════
    // ⭐ v1.2.0: Gapless Pro - NextTrack callback
    // ═══════════════════════════════════════════════════════════════
    
    /**
     * @brief Callback for next track preparation (Gapless Pro)
     * 
     * Called when AudioEngine has pre-loaded the next track and is ready
     * to send the first audio data for gapless buffering in DirettaOutput.
     * 
     * @param data Audio buffer (first chunk of next track)
     * @param numSamples Number of samples (frames)
     * @param format Audio format of next track
     */
    using NextTrackCallback = std::function<void(const uint8_t*, size_t, const AudioFormat&)>;
    
    // ═══════════════════════════════════════════════════════════════

    /**
     * @brief Repeat Modes
     */
    enum class RepeatMode {
        OFF,
        ONE,
        ALL
    };

    /**
     * @brief Constructor
     */
    AudioEngine();
    
    /**
     * @brief Destructor
     */
    ~AudioEngine();
    
    /**
     * @brief Set audio callback
     * @param callback Callback function
     */
    void setAudioCallback(const AudioCallback& callback);
    
    /**
     * @brief Set track change callback
     * @param callback Callback function
     */
    void setTrackChangeCallback(const TrackChangeCallback& callback);

    /**
     * @brief Get current playback track info (real-time from decoder)
     */
    TrackInfo getCurrentTrackInfo() const;

    /**
     * @brief Set track end callback
     * @param callback Callback function
     */
    void setTrackEndCallback(const TrackEndCallback& callback);
    
    // ═══════════════════════════════════════════════════════════════
    // ⭐ v1.2.0: Gapless Pro callback setter
    // ═══════════════════════════════════════════════════════════════
    
    /**
     * @brief Set next track callback for Gapless Pro
     * @param callback Callback function
     */
    void setNextTrackCallback(const NextTrackCallback& callback);
    
    // ═══════════════════════════════════════════════════════════════

    // ⭐ New Control Methods
    void setDirettaTargetIndex(int index);
    void setDirettaTargetIP(const std::string& ip);
    void setVolume(int volume);
    int getVolume() const;
    
    void setRepeatMode(RepeatMode mode);
    RepeatMode getRepeatMode() const;
    
    void setRandomMode(bool enabled);
    bool getRandomMode() const;
    
    // ⭐ AB Loop Controls
    void setABA(double seconds);
    void setABB(double seconds);
    void setABLoop(bool enabled);
    void clearAB();
    bool isABLoopActive() const { return m_abLoopActive; }
    double getABA() const { return m_abA; }
    double getABB() const { return m_abB; }
    
    /**
     * @brief Set current track URI
     * @param uri Track URI
     * @param metadata Track metadata (optional)
     * @param forceReopen Force reopen even if same URI
     */
    void setCurrentURI(const std::string& uri, const std::string& metadata = "", 
                      bool forceReopen = false);

    /**
     * @brief Play a track with specific metadata (including virtual track limits)
     * @param track Track information
     */
    void playTrack(const TrackInfo& track);

    /**
     * @brief Set next track URI for gapless playback
     * @param uri Track URI
     * @param metadata Track metadata (optional)
     */
    void setNextURI(const std::string& uri, const std::string& metadata = "");
    
    /**
     * @brief Set next track fully (preferred for virtual tracks/CUE)
     * @param track Complete track information
     */
    void setNextTrack(const TrackInfo& track);
    
    /**
     * @brief Start playback
     * @return true if successful, false otherwise
     */
    bool play();
    
    /**
     * @brief Stop playback
     */
    void stop();
    
    /**
     * @brief Pause playback
     */
    void pause();
    
    /**
     * @brief Get current state
     * @return Current playback state
     */
    State getState() const { return m_state; }
    
    /**
     * @brief Get current track number
     * @return Track number (1-based)
     */
    int getTrackNumber() const { return m_trackNumber; }
    
    
    /**
     * @brief Get playback position in seconds
     * @return Position in seconds
     */
    double getPosition() const;
    
    /**
     * @brief Seek to a specific position (in seconds)
     * @param seconds Position in seconds
     * @return true if successful, false otherwise
     */
    bool seek(double seconds);
    
    /**
     * @brief Seek to a specific position (time string format)
     * @param timeStr Time string in format "HH:MM:SS", "MM:SS", or seconds as string
     * @return true if successful, false otherwise
     */
    bool seek(const std::string& timeStr);
 
    /**
     * @brief Get current sample rate
     */
    uint32_t getCurrentSampleRate() const;
    
    /**
     * @brief Main processing loop (called from audio thread)
     * @param samplesNeeded Number of samples needed
     * @return true if data produced, false if stopped
     */
    bool process(size_t samplesNeeded);

    // [New] Expose DirettaSync for config
    DirettaSync* getDirettaSync() const { return m_sync.get(); }
    
    // DSD Debug
    void setDsdByteSwap(bool enable) { if (m_sync) m_sync->setDsdByteSwap(enable); }
    void setDsdBitReverse(bool enable) { if (m_sync) m_sync->setDsdBitReverse(enable); }
    
private:
    std::atomic<State> m_state;
    std::atomic<int> m_trackNumber;
    std::atomic<bool> m_pauseRequested{false};
    
    // Current and next track
    std::string m_currentURI;
    std::string m_currentMetadata;
    std::string m_nextURI;
    std::string m_nextMetadata;
    TrackInfo m_currentTrackInfo;
    TrackInfo m_nextTrackInfo;   // ⭐ Storage for virtual properties of next track
    
    // Decoders
    std::unique_ptr<AudioDecoder> m_currentDecoder;
    std::unique_ptr<AudioDecoder> m_nextDecoder;
    
    // Callbacks
    AudioCallback m_audioCallback;
    TrackChangeCallback m_trackChangeCallback;
    TrackEndCallback m_trackEndCallback;
    NextTrackCallback m_nextTrackCallback;  // ⭐ v1.2.0: Gapless Pro
    
    // Synchronization
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    
    // Buffer
    AudioBuffer m_buffer;
    
    // ⭐ Optimized buffer for DSD1024/PCM384 (1MB)
    // Eliminates runtime allocation in audio thread
    alignas(64) uint8_t m_alignedPool[1048576]; 
    
    // ⭐ Decoupled Status for UI (prevents locking audio thread)
    mutable std::mutex m_infoMutex;
    TrackInfo m_publicTrackInfo;
    void updatePublicTrackInfo();
    
    // Playback tracking
    uint64_t m_samplesPlayed;
    int m_silenceCount;  // Pour drainage du buffer Diretta
    bool m_isDraining;   // Flag pour éviter de re-logger "Track finished"
    std::chrono::high_resolution_clock::time_point m_lastUpdateTime;  // For accurate time-based sample calculation
    
    // ⭐ Control State
    std::atomic<int> m_volume{100};
    std::atomic<RepeatMode> m_repeatMode{RepeatMode::OFF};
    std::atomic<bool> m_randomMode{false};
    
    // ⭐ AB Loop State
    std::atomic<bool> m_abLoopActive{false};
    std::atomic<double> m_abA{-1.0};
    std::atomic<double> m_abB{-1.0};
    
    // ⭐ v1.2.0: Gapless Pro state
    bool m_nextTrackPrepared;  // True if next track already sent to DirettaOutput
    
    // Helper functions
    bool openCurrentTrack();
    bool preloadNextTrack();
    void transitionToNextTrack();
    void prepareNextTrackForGapless();  // ⭐ v1.2.0: Gapless Pro helper

    // Thread-safe pending next track mechanism
    mutable std::mutex m_pendingMutex;
    std::atomic<bool> m_pendingNextTrack{false};
    std::string m_pendingNextURI;
    std::string m_pendingNextMetadata;

    // Preload thread management (replaces detached thread)
    std::thread m_preloadThread;
    std::atomic<bool> m_preloadRunning{false};
    void waitForPreloadThread();

    // ⭐⭐⭐ NEW: Async seek mechanism to avoid deadlock
    // The UPnP thread sets these flags, the audio thread processes the seek
    std::atomic<bool> m_seekRequested{false};
    std::atomic<double> m_seekTarget{0.0};

    // [New] Diretta Sync Engine
    std::unique_ptr<DirettaSync> m_sync;

    // Prevent copying
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;
};

#endif // AUDIO_ENGINE_H
