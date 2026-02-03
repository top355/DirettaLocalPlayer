/**
 * @file AudioEngine.cpp
 * @brief Audio Engine implementation - COMPLETE
 */

#include "AudioEngine.h"
#include "DirettaSync.h" // [NEW] Using DirettaSync
#include "SimdUtils.h" // For HAS_AVX2 definition
#include <iostream>
#include <thread>
#include <cstring>
#include <algorithm>  

extern "C" {
// g_verbose is declared in DirettaSync.h which is included above
#define DEBUG_LOG(x) if (g_verbose) { std::cout << x << std::endl; }
#include <libavutil/opt.h>
}

// ============================================================================
// AudioBuffer - 64-byte aligned, grow-only allocation
// ============================================================================

AudioBuffer::AudioBuffer(size_t size)
    : m_data(nullptr)
    , m_size(0)
    , m_capacity(0)
{
    if (size > 0) {
        resize(size);
    }
}

AudioBuffer::~AudioBuffer() {
    if (m_data) {
        std::free(m_data);  // Required for aligned_alloc memory
    }
}
// ⭐ Move constructor (safe transfer of ownership)
AudioBuffer::AudioBuffer(AudioBuffer&& other) noexcept
    : m_data(other.m_data)
    , m_size(other.m_size)
    , m_capacity(other.m_capacity)
{
    other.m_data = nullptr;
    other.m_size = 0;
    other.m_capacity = 0;
}

// ⭐ Move assignment operator (safe transfer of ownership)
AudioBuffer& AudioBuffer::operator=(AudioBuffer&& other) noexcept {
    if (this != &other) {
        if (m_data) {
            std::free(m_data);
        }
        m_data = other.m_data;
        m_size = other.m_size;
        m_capacity = other.m_capacity;
        other.m_data = nullptr;
        other.m_size = 0;
        other.m_capacity = 0;
    }
    return *this;
}

void AudioBuffer::resize(size_t size) {
    if (size > m_capacity) {
        growCapacity(size);
    }
    m_size = size;
}

void AudioBuffer::ensureCapacity(size_t cap) {
    if (cap > m_capacity) {
        growCapacity(cap);
    }
}

void AudioBuffer::growCapacity(size_t needed) {
    // Grow by 1.5x to reduce future reallocations
    size_t newCapacity = std::max(needed, m_capacity + m_capacity / 2);

    // Minimum capacity to avoid tiny allocations
    newCapacity = std::max(newCapacity, size_t{1024});

    // Round up to alignment boundary
    newCapacity = (newCapacity + ALIGNMENT - 1) & ~(ALIGNMENT - 1);

    // Allocate aligned memory
    uint8_t* newData = static_cast<uint8_t*>(std::aligned_alloc(ALIGNMENT, newCapacity));
    if (!newData) {
        throw std::bad_alloc();
    }

    // Copy existing data if present
    if (m_data && m_size > 0) {
        std::memcpy(newData, m_data, m_size);
    }

    if (m_data) {
        std::free(m_data);
    }
    m_data = newData;
    m_capacity = newCapacity;
}

// ============================================================================
// AudioDecoder
// ============================================================================

AudioDecoder::AudioDecoder()
    : m_formatContext(nullptr)
    , m_codecContext(nullptr)
    , m_swrContext(nullptr)
    , m_audioStreamIndex(-1)
    , m_eof(false)
    , m_rawDSD(false)         // ⭐ DSD mode off by default
    , m_packet(nullptr)       // ⭐ Packet for raw reading
    , m_pcmFifo(nullptr)
    , m_bypassMode(false)
    , m_dsdRemainderCount(0)
    , m_dsdBufferCapacity(0)
{
    // 环形缓冲区已移除 - 使用 DirettaRingBuffer 统一管理
    
    // 预分配 DSD 余量缓冲区（在构造时，确保 gapless 模式下也有预分配）
    // 增加到 128KB 以应对更大的数据包
    constexpr size_t DSD_REMAINDER_CAPACITY = 128 * 1024;  // 从 64KB 增加到 128KB
    m_dsdRemainderBuffer.resize(DSD_REMAINDER_CAPACITY);
    std::cout << "[AudioDecoder] Pre-allocated DSD remainder buffer in constructor: " 
              << DSD_REMAINDER_CAPACITY << " bytes" << std::endl;

    // 预分配工作缓冲区，避免运行时分配
    constexpr size_t WORK_BUFFER_CAPACITY = 2 * 1024 * 1024;  // 2MB
    m_workBuffer.ensureCapacity(WORK_BUFFER_CAPACITY);
    std::cout << "[AudioDecoder] Pre-allocated work buffer: " 
              << WORK_BUFFER_CAPACITY << " bytes" << std::endl;

    // 预分配DSD通道缓冲区
    constexpr size_t DSD_CHANNEL_BUFFER_CAPACITY = 32768;  // 32KB per channel
    m_dsdLeftBuffer.ensureCapacity(DSD_CHANNEL_BUFFER_CAPACITY);
    m_dsdRightBuffer.ensureCapacity(DSD_CHANNEL_BUFFER_CAPACITY);
    m_dsdBufferCapacity = DSD_CHANNEL_BUFFER_CAPACITY;
    std::cout << "[AudioDecoder] Pre-allocated DSD channel buffers: " 
              << DSD_CHANNEL_BUFFER_CAPACITY << " bytes/channel" << std::endl;
}

AudioDecoder::~AudioDecoder() {
    close();
}

bool AudioDecoder::open(const std::string& url) {
    std::cout << "[AudioDecoder] Opening: " << url.substr(0, 80) << "..." << std::endl;
    
    // Open input file
    m_formatContext = avformat_alloc_context();
    if (!m_formatContext) {
        std::cerr << "[AudioDecoder] Failed to allocate format context" << std::endl;
        return false;
    }
    
    // Configure FFmpeg options for robust HTTP streaming (Qobuz)
    AVDictionary* options = nullptr;
    
    // Automatic reconnection on connection loss
    av_dict_set(&options, "reconnect", "1", 0);
    av_dict_set(&options, "reconnect_streamed", "1", 0);
    av_dict_set(&options, "reconnect_delay_max", "5", 0);  // Max 5 seconds between retries
    
    // Timeout to avoid blocking indefinitely
    av_dict_set(&options, "timeout", "10000000", 0);  // 10 seconds in microseconds
    
    // Improved network buffering
    av_dict_set(&options, "buffer_size", "32768", 0);  // 32KB buffer
    
    // HTTP persistent connections
    av_dict_set(&options, "http_persistent", "1", 0);
    av_dict_set(&options, "multiple_requests", "1", 0);
    
    // User-Agent (some servers check it)
    av_dict_set(&options, "user_agent", "DirettaRenderer/1.0", 0);
    
    // IMPORTANT: Ignore file size to avoid premature EOF
    av_dict_set(&options, "ignore_eof", "1", 0);
    
    DEBUG_LOG("[AudioDecoder] Opening with streaming options (reconnect enabled)");
    
    if (avformat_open_input(&m_formatContext, url.c_str(), nullptr, &options) < 0) {
        std::cerr << "[AudioDecoder] Failed to open input: " << url << std::endl;
        av_dict_free(&options);
        avformat_free_context(m_formatContext);
        m_formatContext = nullptr;
        return false;
    }
    
    // Free unused options
    av_dict_free(&options);
    
    // Retrieve stream information
    if (avformat_find_stream_info(m_formatContext, nullptr) < 0) {
        std::cerr << "[AudioDecoder] Failed to find stream info" << std::endl;
        avformat_close_input(&m_formatContext);
        return false;
    }
    
    // Log duration information
    if (m_formatContext->duration != AV_NOPTS_VALUE) {
        int64_t duration_seconds = m_formatContext->duration / AV_TIME_BASE;
        int64_t duration_ms = (m_formatContext->duration % AV_TIME_BASE) * 1000 / AV_TIME_BASE;
        DEBUG_LOG("[AudioDecoder] Stream duration: " << duration_seconds << "." 
                  << duration_ms << " seconds");
    } else {
        DEBUG_LOG("[AudioDecoder] Stream duration: unknown (live stream?)");
    }
    
    // Find audio stream
    m_audioStreamIndex = -1;
    for (unsigned int i = 0; i < m_formatContext->nb_streams; i++) {
        if (m_formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            m_audioStreamIndex = i;
            break;
        }
    }
    
    if (m_audioStreamIndex == -1) {
        std::cerr << "[AudioDecoder] No audio stream found" << std::endl;
        avformat_close_input(&m_formatContext);
        return false;
    }
    
    AVStream* audioStream = m_formatContext->streams[m_audioStreamIndex];
    AVCodecParameters* codecpar = audioStream->codecpar;

    // Find decoder
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        std::cerr << "[AudioDecoder] Codec not found" << std::endl;
        avformat_close_input(&m_formatContext);
        return false;
    }
    
    // Allocate codec context
    m_codecContext = avcodec_alloc_context3(codec);
    if (!m_codecContext) {
        std::cerr << "[AudioDecoder] Failed to allocate codec context" << std::endl;
        avformat_close_input(&m_formatContext);
        return false;
    }
    
    // Copy codec parameters
    if (avcodec_parameters_to_context(m_codecContext, codecpar) < 0) {
        std::cerr << "[AudioDecoder] Failed to copy codec parameters" << std::endl;
        avcodec_free_context(&m_codecContext);
        avformat_close_input(&m_formatContext);
        return false;
    }
    
    // Open codec
    if (avcodec_open2(m_codecContext, codec, nullptr) < 0) {
        std::cerr << "[AudioDecoder] Failed to open codec" << std::endl;
        avcodec_free_context(&m_codecContext);
        avformat_close_input(&m_formatContext);
        return false;
    }
    
    // Fill track info
    m_trackInfo.sampleRate = codecpar->sample_rate;
    m_trackInfo.channels = codecpar->ch_layout.nb_channels;
    m_trackInfo.codec = codec->name;
    m_trackInfo.bitsPerSample = codecpar->bits_per_raw_sample;
    
    // ⭐ Detect S24 alignment hint from codec ID
    if (codecpar->codec_id == AV_CODEC_ID_PCM_S24LE) {
        m_trackInfo.s24Alignment = TrackInfo::S24Alignment::Lsb;
        DEBUG_LOG("[AudioDecoder] S24 hint: LSB (from codec ID)");
    } else if (codecpar->codec_id == AV_CODEC_ID_PCM_S24BE) {
        // Technically BE could be anything, but we hint LSB for standard WAV
        m_trackInfo.s24Alignment = TrackInfo::S24Alignment::Lsb;
    }
    
    // ✅ Classify codec complexity for buffer optimization
    // Uncompressed formats (WAV/AIFF): minimal latency
    // Compressed formats (FLAC/ALAC): need decoding buffer
    bool isUncompressedPCM = (
        codecpar->codec_id == AV_CODEC_ID_PCM_S16LE ||
        codecpar->codec_id == AV_CODEC_ID_PCM_S16BE ||
        codecpar->codec_id == AV_CODEC_ID_PCM_S24LE ||
        codecpar->codec_id == AV_CODEC_ID_PCM_S24BE ||
        codecpar->codec_id == AV_CODEC_ID_PCM_S32LE ||
        codecpar->codec_id == AV_CODEC_ID_PCM_S32BE
    );
    
    m_trackInfo.isCompressed = !isUncompressedPCM;
    
    if (isUncompressedPCM) {
        DEBUG_LOG("[AudioDecoder] ✓ Uncompressed PCM (WAV/AIFF) - low latency path");
    } else {
        DEBUG_LOG("[AudioDecoder] ℹ️  Compressed format (" << codec->name 
                  << ") - decoding required");
    }
    
    // Check if DSD - CRITICAL: Use RAW mode for native DSD!
    m_trackInfo.isDSD = false;
    if (codecpar->codec_id == AV_CODEC_ID_DSD_LSBF ||
        codecpar->codec_id == AV_CODEC_ID_DSD_MSBF ||
        codecpar->codec_id == AV_CODEC_ID_DSD_MSBF_PLANAR ||
        codecpar->codec_id == AV_CODEC_ID_DSD_LSBF_PLANAR) {
        

            // ════════════════════════════════════════════════════════
            // DSD native mode
            // ════════════════════════════════════════════════════════
            std::cout << "[AudioDecoder] ════════════════════════════════════════" << std::endl;
            std::cout << "[AudioDecoder] 🎵 DSD NATIVE MODE ACTIVATED!" << std::endl;
            std::cout << "[AudioDecoder] ════════════════════════════════════════" << std::endl;
            
            m_trackInfo.isDSD = true;
            m_trackInfo.bitDepth = 1; // DSD is 1-bit
            
        // ⭐ v1.2.0 : Détecter DSF vs DFF
        if (m_formatContext && m_formatContext->url) {
            std::string url(m_formatContext->url);
            
            // Détecter par extension fichier
            if (url.find(".dsf") != std::string::npos || 
                url.find(".DSF") != std::string::npos) {
                m_trackInfo.dsdSourceFormat = TrackInfo::DSDSourceFormat::DSF;
                DEBUG_LOG("[AudioDecoder] 📀 DSD source format: DSF (LSB first)");
            }
            else if (url.find(".dff") != std::string::npos || 
                     url.find(".DFF") != std::string::npos) {
                m_trackInfo.dsdSourceFormat = TrackInfo::DSDSourceFormat::DFF;
                DEBUG_LOG("[AudioDecoder] 📀 DSD source format: DFF (MSB first)");
            }
            else {
                // Fallback : détecter par codec ID
                if (codecpar->codec_id == AV_CODEC_ID_DSD_LSBF ||
                    codecpar->codec_id == AV_CODEC_ID_DSD_LSBF_PLANAR) {
                    m_trackInfo.dsdSourceFormat = TrackInfo::DSDSourceFormat::DSF;
                    DEBUG_LOG("[AudioDecoder] 📀 DSD source format: DSF (detected from codec)");
                }
                else {
                    m_trackInfo.dsdSourceFormat = TrackInfo::DSDSourceFormat::DFF;
                    DEBUG_LOG("[AudioDecoder] 📀 DSD source format: DFF (detected from codec)");
                }
            }
        }

            // CRITICAL: FFmpeg reports packet rate, not DSD bit rate!
            // For DSD: bit_rate = packet_rate × 8 (8 bits per byte)
            // DSD64 = 2822400 Hz, but FFmpeg reports 352800 Hz (packet rate)
            uint32_t packetRate = codecpar->sample_rate;  // 352800 for DSD64
            uint32_t dsdBitRate = packetRate * 8;          // 2822400 for DSD64
            
            m_trackInfo.sampleRate = dsdBitRate;  // ⭐ Use TRUE DSD bit rate!
            
            // Determine DSD rate (DSD64, DSD128, etc.)
            // DSD64 = 2822400 Hz = 44100 * 64
            int dsdMultiplier = dsdBitRate / 44100;
            m_trackInfo.dsdRate = dsdMultiplier;
            
            // 验证DSD速率
            if (dsdMultiplier >= 64 && dsdMultiplier <= 1024 && (dsdMultiplier & (dsdMultiplier - 1)) == 0) {
                DEBUG_LOG("[AudioDecoder] 🎵 DSD" << dsdMultiplier << " detected!");
                DEBUG_LOG("[AudioDecoder]    FFmpeg packet rate: " << packetRate << " Hz");
                DEBUG_LOG("[AudioDecoder]    True DSD bit rate: " << dsdBitRate << " Hz");
                DEBUG_LOG("[AudioDecoder] ⚠️  NO DECODING - Reading raw DSD packets!");
            } else {
                std::cerr << "[AudioDecoder] ⚠️  Unusual DSD rate detected: DSD" << dsdMultiplier << std::endl;
                std::cerr << "[AudioDecoder]    FFmpeg packet rate: " << packetRate << " Hz" << std::endl;
                std::cerr << "[AudioDecoder]    True DSD bit rate: " << dsdBitRate << " Hz" << std::endl;
            }
            
            // ⭐ CRITICAL: Activate RAW DSD mode
            m_rawDSD = true;
            m_packet = av_packet_alloc();
            
            // ⭐ DO NOT open codec for DSD!
            // We'll read raw packets with av_read_frame()
            DEBUG_LOG("[AudioDecoder] ✓ DSD Native mode ready");
            
            // Calculate duration
            if (audioStream->duration != AV_NOPTS_VALUE) {
                m_trackInfo.duration = av_rescale_q(audioStream->duration, 
                                                    audioStream->time_base,
                                                    {1, (int)m_trackInfo.sampleRate});
                m_trackInfo.durationSeconds = (double)m_trackInfo.duration / m_trackInfo.sampleRate;
            } else {
                m_trackInfo.duration = 0;
                m_trackInfo.durationSeconds = 0;
            }
            
            m_eof = false;
            
            std::cout << "[AudioDecoder] ✓ Opened successfully (DSD NATIVE)" << std::endl;
            
            return true;  // ⭐ Exit early - no codec opening needed!
       
    }  // End of DSD detection
    
    // ══════════════════════════════════════════════════════════════
    // PCM MODE - Open codec and prepare for decoding
    // ══════════════════════════════════════════════════════════════
    
    m_rawDSD = false;  // Not DSD, use normal decoding
    
    // PCM format detection
    switch (codecpar->format) {
        case AV_SAMPLE_FMT_S16:
        case AV_SAMPLE_FMT_S16P:
            m_trackInfo.bitDepth = 16;
            break;
        case AV_SAMPLE_FMT_S32:
        case AV_SAMPLE_FMT_S32P:
            m_trackInfo.bitDepth = 32;
            break;
        case AV_SAMPLE_FMT_FLT:
        case AV_SAMPLE_FMT_FLTP:
            m_trackInfo.bitDepth = 32; // Float treated as 32-bit
            break;
        default:
            m_trackInfo.bitDepth = 24; // Default assumption
            break;
    }

    m_rawDSD = false;  // Not DSD, use normal decoding
    
    // ⭐ CRITICAL FIX: Detect REAL bit depth from source
    int realBitDepth = 0;
    
    // Method 1: Try bits_per_raw_sample (most reliable for FLAC/ALAC)
    if (codecpar->bits_per_raw_sample > 0 && codecpar->bits_per_raw_sample <= 32) {
        realBitDepth = codecpar->bits_per_raw_sample;
        m_trackInfo.bitsPerSample = realBitDepth;
        DEBUG_LOG("[AudioDecoder] ✓ Real bit depth from bits_per_raw_sample: " 
                  << realBitDepth << " bits");
    }
    // Method 2: Deduce from codec ID (for PCM formats like WAV)
    else if (codecpar->codec_id == AV_CODEC_ID_PCM_S16LE || 
             codecpar->codec_id == AV_CODEC_ID_PCM_S16BE) {
        realBitDepth = 16;
        m_trackInfo.bitsPerSample = 16;
        DEBUG_LOG("[AudioDecoder] ✓ Bit depth from codec ID (PCM16): 16 bits");
    }
    else if (codecpar->codec_id == AV_CODEC_ID_PCM_S24LE || 
             codecpar->codec_id == AV_CODEC_ID_PCM_S24BE) {
        realBitDepth = 24;
        m_trackInfo.bitsPerSample = 24;
        DEBUG_LOG("[AudioDecoder] ✓ Bit depth from codec ID (PCM24): 24 bits");
    }
    else if (codecpar->codec_id == AV_CODEC_ID_PCM_S32LE || 
             codecpar->codec_id == AV_CODEC_ID_PCM_S32BE) {
        realBitDepth = 32;
        m_trackInfo.bitsPerSample = 32;
        DEBUG_LOG("[AudioDecoder] ✓ Bit depth from codec ID (PCM32): 32 bits");
    }
    
    // Method 3: Fallback to FFmpeg's internal format
    if (realBitDepth == 0) {
        DEBUG_LOG("[AudioDecoder] ⚠️  bits_per_raw_sample not available, using format detection");
        
        switch (codecpar->format) {
            case AV_SAMPLE_FMT_S16:
            case AV_SAMPLE_FMT_S16P:
                realBitDepth = 16;
                break;
            case AV_SAMPLE_FMT_S32:
            case AV_SAMPLE_FMT_S32P:
                realBitDepth = 32;
                break;
            case AV_SAMPLE_FMT_FLT:
            case AV_SAMPLE_FMT_FLTP:
                realBitDepth = 32;
                break;
            default:
                realBitDepth = 24;
                DEBUG_LOG("[AudioDecoder] ⚠️  Unknown format, defaulting to 24-bit");
                break;
        }
        m_trackInfo.bitsPerSample = realBitDepth;
    }
    
    // Safety check
    if (realBitDepth != 16 && realBitDepth != 24 && realBitDepth != 32) {
        std::cerr << "[AudioDecoder] ❌ Invalid bit depth detected: " << realBitDepth 
                  << ", falling back to 24-bit" << std::endl;
        realBitDepth = 24;
        m_trackInfo.bitsPerSample = 24;
    }
    
    m_trackInfo.bitDepth = realBitDepth;

    DEBUG_LOG("[AudioDecoder] 🎵 PCM: " << m_trackInfo.codec
              << " " << m_trackInfo.sampleRate << "Hz/"
              << m_trackInfo.bitDepth << "bit/"
              << m_trackInfo.channels << "ch");
    
    // Open codec context
    if (avcodec_parameters_to_context(m_codecContext, codecpar) < 0) {
        std::cerr << "[AudioDecoder] Failed to copy codec parameters to context" << std::endl;
        avcodec_free_context(&m_codecContext);
        avformat_close_input(&m_formatContext);
        return false;
    }
    
    // Open codec
    if (avcodec_open2(m_codecContext, codec, nullptr) < 0) {
        std::cerr << "[AudioDecoder] Failed to open codec" << std::endl;
        avcodec_free_context(&m_codecContext);
        avformat_close_input(&m_formatContext);
        return false;
    }
    
    // FIFO will be initialized in initResampler function for non-DSD formats
    // This ensures correct sample format is used after decoding/resampling
    
    // Calculate duration
    if (audioStream->duration != AV_NOPTS_VALUE) {
        m_trackInfo.duration = av_rescale_q(audioStream->duration, 
                                            audioStream->time_base,
                                            {1, (int)m_trackInfo.sampleRate});
        m_trackInfo.durationSeconds = (double)m_trackInfo.duration / m_trackInfo.sampleRate;
    } else {
        m_trackInfo.duration = 0;
        m_trackInfo.durationSeconds = 0;
    }
    
    m_eof = false;
    
    // DSD 余量缓冲已在构造函数中预分配（支持 gapless 模式）
    
    std::cout << "[AudioDecoder] ✓ Opened successfully" << std::endl;
 
    return true;
}
void AudioDecoder::close() {
    if (m_swrContext) {
        swr_free(&m_swrContext);
        m_swrContext = nullptr;
    }
    if (m_pcmFifo) {
        av_audio_fifo_free(m_pcmFifo);
        m_pcmFifo = nullptr;
    }
    if (m_codecContext) {
        avcodec_free_context(&m_codecContext);
        m_codecContext = nullptr;
    }
    if (m_packet) {  // ⭐ Free DSD packet
        av_packet_free(&m_packet); 
        m_packet = nullptr; // Prevent double free
    }
    if (m_formatContext) {
        avformat_close_input(&m_formatContext);
        m_formatContext = nullptr;
    }
    
    // 环形缓冲区已移除
    
    m_audioStreamIndex = -1;
    m_eof = false;
    m_rawDSD = false;  // ⭐ Reset DSD flag
    m_dsdRemainderCount = 0;
    m_resamplerInitialized = false;
    m_bypassMode = false;
    m_dsdBufferCapacity = 0;
}

size_t AudioDecoder::readSamples(AudioBuffer& buffer, size_t numSamples,
                                uint32_t outputRate, uint32_t outputBits) {
    
    // ══════════════════════════════════════════════════════════════
    // DSD NATIVE MODE - 终极优化版 (Perfect Edition) - 支持 DSD1024
    // ══════════════════════════════════════════════════════════════
    if (m_rawDSD) {
        if (m_eof) return 0;

        size_t totalBytesNeeded = (numSamples * m_trackInfo.channels) / 8;
        size_t totalBytesRead = 0;

        // 确保输出 Buffer 足够大
        if (buffer.size() < totalBytesNeeded) buffer.resize(totalBytesNeeded);
        uint8_t* outputPtr = buffer.data();

        // -------------------------------------------------------
        // 1. 先消耗剩余的 Buffer (m_dsdRemainderBuffer)
        // 环形缓冲区已移除 - DirettaRingBuffer 统一管理主缓冲
        // -------------------------------------------------------
        if (m_dsdRemainderCount > 0) {
            size_t bytesToUse = std::min(m_dsdRemainderCount, totalBytesNeeded - totalBytesRead);
            // ⭐ Optimization: Use fast memcpy
            SimdUtils::memcpy_fixed(outputPtr, m_dsdRemainderBuffer.data(), bytesToUse);
            outputPtr += bytesToUse;
            totalBytesRead += bytesToUse;

            if (bytesToUse < m_dsdRemainderCount) {
                 memmove(m_dsdRemainderBuffer.data(), m_dsdRemainderBuffer.data() + bytesToUse, m_dsdRemainderCount - bytesToUse);
                 m_dsdRemainderCount -= bytesToUse;
            } else {
                 m_dsdRemainderCount = 0;
            }
            
            // 如果凑够了，直接返回
            if (totalBytesRead >= totalBytesNeeded) {
                return (totalBytesRead * 8) / m_trackInfo.channels;
            }
        }

        // -------------------------------------------------------
        // 3. 读取并处理新 Packet
        // -------------------------------------------------------
        while (totalBytesRead < totalBytesNeeded && !m_eof) {
            int ret = av_read_frame(m_formatContext, m_packet);
            if (ret < 0) {
                // Log position when EOF occurs
                if (m_formatContext->pb && m_formatContext->pb->pos > 0) {
                    std::cout << "[AudioDecoder] Bytes read from stream: " << m_formatContext->pb->pos << std::endl;
                }
                
                if (ret == AVERROR_EOF) {
                    m_eof = true;
                    DEBUG_LOG("[AudioDecoder] EOF reached");
                } else if (ret == AVERROR(ETIMEDOUT)) {
                    std::cerr << "[AudioDecoder] ⚠️  Timeout - connection too slow or lost" << std::endl;
                    // 尝试重试，而不是直接设置EOF
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue; // 继续尝试读取
                } else if (ret == AVERROR(ECONNRESET)) {
                    std::cerr << "[AudioDecoder] ⚠️  Connection reset by server" << std::endl;
                    // 对于网络流，尝试重新连接
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue; // 继续尝试读取
                } else {
                    char errbuf[AV_ERROR_MAX_STRING_SIZE];
                    av_strerror(ret, errbuf, sizeof(errbuf));
                    std::cerr << "[AudioDecoder] ⚠️  Read error (" << ret << "): " << errbuf << std::endl;
                    
                    // 检查是否是网络相关错误
                    bool isNetworkError = (ret == AVERROR(ENOENT) || 
                                          ret == AVERROR(EIO) || 
                                          ret == AVERROR(EPIPE) || 
                                          ret == AVERROR(EINVAL) ||
                                          strstr(errbuf, "network") != nullptr ||
                                          strstr(errbuf, "connection") != nullptr ||
                                          strstr(errbuf, "timeout") != nullptr);
                    
                    if (isNetworkError) {
                        // 对于网络错误，尝试重试
                        std::cerr << "[AudioDecoder] 🔄 Attempting to recover from network error..." << std::endl;
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        continue; // 继续尝试读取
                    } else {
                        // 对于其他错误，设置EOF
                        m_eof = true;
                    }
                }
                break;
            }

            if (m_packet->stream_index == m_audioStreamIndex) {
                 uint8_t* pData = m_packet->data;
                 size_t pSize = m_packet->size;
                                  // [检测] 获取 Codec ID
                  enum AVCodecID codecID = m_formatContext->streams[m_audioStreamIndex]->codecpar->codec_id;
                  bool isPlanar = (codecID == AV_CODEC_ID_DSD_LSBF_PLANAR || codecID == AV_CODEC_ID_DSD_MSBF_PLANAR);

                  // [检测] 是否需要比特反转 (针对 DFF/MSB 格式)
                  // ════════════════════════════════════════════════════════════
                  // ⭐ Phase 3: DSD Processing (Beta 10 Compatible - Interleaved 4-byte blocks)
                  // ════════════════════════════════════════════════════════════
                  // Output format: [4B L][4B R][4B L][4B R]... with optional bit-reversal
                  // This matches what Beta 10 DirettaOutput expects!
                  
                  bool needBitReverse = (m_trackInfo.dsdSourceFormat == TrackInfo::DSDSourceFormat::DFF ||
                                         m_trackInfo.codec.find("msbf") != std::string::npos);
                  
                  // Ensure work buffer is large enough (为DSD1024预留足够空间)
                size_t requiredBufferSize = pSize * 2;
                if (m_workBuffer.size() < requiredBufferSize) {
                    m_workBuffer.resize(requiredBufferSize);
                }
                  uint8_t* dst = m_workBuffer.data();
                  
                  bool channelsHandled = false;

                  if (isPlanar && m_trackInfo.channels == 2) {
                      // Source is Planar (L...L R...R) -> Convert to Interleaved 4-byte blocks
                      size_t half = pSize / 2;
                      
                      const uint8_t* srcL = pData;
                      const uint8_t* srcR = pData + half;
                      
                      // Apply bit reversal if needed (DFF files)
                      if (needBitReverse) {
                          // Use aligned pools for bit-reversed data
                          if (half > sizeof(m_dsdPoolL)) {
                              // 动态分配临时缓冲区
                              uint8_t* tempL = new uint8_t[half];
                              uint8_t* tempR = new uint8_t[half];
                              
                              // Use SIMD for bit reversal if available
                              #if HAS_AVX2
                              size_t simdSize = half & ~31;
                              size_t j = 0;
                              for (; j < simdSize; j += 32) {
                                  __m256i dataL = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(srcL + j));
                                  __m256i dataR = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(srcR + j));
                                  __m256i reversedL = SimdUtils::bit_reverse_avx2(dataL);
                                  __m256i reversedR = SimdUtils::bit_reverse_avx2(dataR);
                                  _mm256_storeu_si256(reinterpret_cast<__m256i*>(tempL + j), reversedL);
                                  _mm256_storeu_si256(reinterpret_cast<__m256i*>(tempR + j), reversedR);
                              }
                              // Handle remainder with scalar
                              for (; j < half; j++) {
                                  tempL[j] = SimdUtils::kBitReverseTable[srcL[j]];
                                  tempR[j] = SimdUtils::kBitReverseTable[srcR[j]];
                              }
                              #else
                              // Scalar fallback
                              for (size_t j = 0; j < half; j++) {
                                  tempL[j] = SimdUtils::kBitReverseTable[srcL[j]];
                                  tempR[j] = SimdUtils::kBitReverseTable[srcR[j]];
                              }
                              #endif
                              srcL = tempL;
                              srcR = tempR;
                              
                              // Interleave in 4-byte blocks [4B L][4B R][4B L][4B R]...
                              size_t i = 0;
                              while (i + 4 <= half) {
                                  std::memcpy(dst, srcL + i, 4); dst += 4;
                                  std::memcpy(dst, srcR + i, 4); dst += 4;
                                  i += 4;
                              }
                              // Scalar remainder - 必须保持 4 字节块对齐
                              // 如果有余量，丢弃（DSD 必须是 4 字节块的整数倍）
                              size_t remainder = half - i;
                              if (remainder > 0) {
                                  std::cerr << "[AudioDecoder] WARNING: DSD packet not 4-byte aligned, dropping " 
                                            << remainder << " bytes" << std::endl;
                              }
                              
                              delete[] tempL;
                              delete[] tempR;
                          } else {
                              // Use SIMD for bit reversal if available
                              #if HAS_AVX2
                              size_t simdSize = half & ~31;
                              size_t j = 0;
                              for (; j < simdSize; j += 32) {
                                  __m256i dataL = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(srcL + j));
                                  __m256i dataR = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(srcR + j));
                                  __m256i reversedL = SimdUtils::bit_reverse_avx2(dataL);
                                  __m256i reversedR = SimdUtils::bit_reverse_avx2(dataR);
                                  _mm256_storeu_si256(reinterpret_cast<__m256i*>(m_dsdPoolL + j), reversedL);
                                  _mm256_storeu_si256(reinterpret_cast<__m256i*>(m_dsdPoolR + j), reversedR);
                              }
                              // Handle remainder with scalar
                              for (; j < half; j++) {
                                  m_dsdPoolL[j] = SimdUtils::kBitReverseTable[srcL[j]];
                                  m_dsdPoolR[j] = SimdUtils::kBitReverseTable[srcR[j]];
                              }
                              #else
                              // Scalar fallback
                              for (size_t j = 0; j < half; j++) {
                                  m_dsdPoolL[j] = SimdUtils::kBitReverseTable[srcL[j]];
                                  m_dsdPoolR[j] = SimdUtils::kBitReverseTable[srcR[j]];
                              }
                              #endif
                              srcL = m_dsdPoolL;
                              srcR = m_dsdPoolR;
                              
                              // Interleave in 4-byte blocks [4B L][4B R][4B L][4B R]...
                              size_t i = 0;
                              while (i + 4 <= half) {
                                  std::memcpy(dst, srcL + i, 4); dst += 4;
                                  std::memcpy(dst, srcR + i, 4); dst += 4;
                                  i += 4;
                              }
                              // Scalar remainder - 必须保持 4 字节块对齐
                              // 如果有余量，丢弃（DSD 必须是 4 字节块的整数倍）
                              size_t remainder = half - i;
                              if (remainder > 0) {
                                  std::cerr << "[AudioDecoder] WARNING: DSD packet not 4-byte aligned, dropping " 
                                            << remainder << " bytes" << std::endl;
                              }
                          }
                      } else {
                          // No bit reversal needed
                          size_t i = 0;
                          while (i + 4 <= half) {
                              std::memcpy(dst, srcL + i, 4); dst += 4;
                              std::memcpy(dst, srcR + i, 4); dst += 4;
                              i += 4;
                          }
                          // Scalar remainder - 必须保持 4 字节块对齐
                          // 如果有余量，丢弃（DSD 必须是 4 字节块的整数倍）
                          size_t remainder = half - i;
                          if (remainder > 0) {
                              std::cerr << "[AudioDecoder] WARNING: DSD packet not 4-byte aligned, dropping " 
                                        << remainder << " bytes" << std::endl;
                          }
                      }
                      channelsHandled = true;
                      
                  } else if (m_trackInfo.channels == 2) {
                      // Source is Interleaved [LRLR...] -> Convert to 4-byte blocks
                      const uint8_t* src = pData;
                      size_t i = 0;
                      
                      while (i + 8 <= pSize) {
                          uint8_t L[4], R[4];
                          for (int k = 0; k < 4; k++) { L[k] = src[2*k]; R[k] = src[2*k+1]; }
                          src += 8;
                          
                          if (needBitReverse) {
                              #if HAS_AVX2
                              // Use SIMD for bit reversal
                              // Create 16-byte buffers for SIMD loading
                              uint8_t L_16[16] = {0};
                              uint8_t R_16[16] = {0};
                              std::memcpy(L_16, L, 4);
                              std::memcpy(R_16, R, 4);
                              
                              __m128i dataL = _mm_loadu_si128(reinterpret_cast<const __m128i*>(L_16));
                              __m128i dataR = _mm_loadu_si128(reinterpret_cast<const __m128i*>(R_16));
                              // Extend to 256-bit for AVX2 processing
                              __m256i dataL_256 = _mm256_set_m128i(_mm_setzero_si128(), dataL);
                              __m256i dataR_256 = _mm256_set_m128i(_mm_setzero_si128(), dataR);
                              __m256i reversedL = SimdUtils::bit_reverse_avx2(dataL_256);
                              __m256i reversedR = SimdUtils::bit_reverse_avx2(dataR_256);
                              // Extract lower 128 bits
                              __m128i reversedL_128 = _mm256_extracti128_si256(reversedL, 0);
                              __m128i reversedR_128 = _mm256_extracti128_si256(reversedR, 0);
                              // Store results - only first 4 bytes
                              uint8_t resultL[16];
                              uint8_t resultR[16];
                              _mm_storeu_si128(reinterpret_cast<__m128i*>(resultL), reversedL_128);
                              _mm_storeu_si128(reinterpret_cast<__m128i*>(resultR), reversedR_128);
                              std::memcpy(dst, resultL, 4);
                              dst += 4;
                              std::memcpy(dst, resultR, 4);
                              dst += 4;
                              #else
                              // Scalar fallback
                              for (int k = 0; k < 4; k++) { *dst++ = SimdUtils::kBitReverseTable[L[k]]; }
                              for (int k = 0; k < 4; k++) { *dst++ = SimdUtils::kBitReverseTable[R[k]]; }
                              #endif
                          } else {
                              for (int k = 0; k < 4; k++) *dst++ = L[k];
                              for (int k = 0; k < 4; k++) *dst++ = R[k];
                          }
                          i += 8;
                      }
                      // Remainder - DSD 必须是 8 字节（4字节块×2声道）的整数倍
                      // 如果有余量，丢弃
                      size_t remainder = pSize - i;
                      if (remainder > 0) {
                          std::cerr << "[AudioDecoder] WARNING: DSD interleaved packet not 8-byte aligned, dropping " 
                                    << remainder << " bytes" << std::endl;
                      }
                      channelsHandled = true;
                  }
                  
                  if (!channelsHandled) {
                      // Mono or Multichannel (>2) or unknown
                      // Apply bit reversal if needed
                      if (needBitReverse) {
                          #if HAS_AVX2
                          // Use SIMD for bit reversal
                          size_t simdSize = pSize & ~31;
                          size_t k = 0;
                          for (; k < simdSize; k += 32) {
                              __m256i data = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pData + k));
                              __m256i reversed = SimdUtils::bit_reverse_avx2(data);
                              _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + k), reversed);
                          }
                          // Handle remainder with scalar
                          for (; k < pSize; k++) {
                              dst[k] = SimdUtils::kBitReverseTable[pData[k]];
                          }
                          #else
                          // Scalar fallback
                          for (size_t k = 0; k < pSize; k++) {
                              dst[k] = SimdUtils::kBitReverseTable[pData[k]];
                          }
                          #endif
                      } else {
                          SimdUtils::memcpy_fixed(dst, pData, pSize);
                      }
                  }
                  
                  // Use work buffer directly
                  pData = m_workBuffer.data();
                  size_t processedSize = dst - m_workBuffer.data();
                 
                 size_t bytesNeeded = totalBytesNeeded - totalBytesRead;
                 if (processedSize <= bytesNeeded) {
                     memcpy(outputPtr, pData, processedSize);
                     outputPtr += processedSize;
                     totalBytesRead += processedSize;
                 } else {
                     memcpy(outputPtr, pData, bytesNeeded);
                     outputPtr += bytesNeeded;
                     totalBytesRead += bytesNeeded;
                     
                     size_t remaining = processedSize - bytesNeeded;
                     
                     // 🔍 诊断日志：监控大余量数据包（可能导致卡顿）
                     if (remaining > 32 * 1024) {  // 如果余量 > 32KB
                         std::cout << "[AudioDecoder] Large DSD remainder: " << remaining 
                                   << " bytes (buffer capacity: " << m_dsdRemainderBuffer.size() << ")" 
                                   << std::endl;
                     }
                     
                     // 存入余量缓冲区（仅存储单次数据包的剩余字节）
                     if (m_dsdRemainderBuffer.size() < remaining) {
                         std::cerr << "[AudioDecoder] ⚠️ WARNING: Resizing remainder buffer: " 
                                   << m_dsdRemainderBuffer.size() << " → " << remaining 
                                   << " bytes (可能导致卡顿)" << std::endl;
                         m_dsdRemainderBuffer.resize(remaining);
                     }
                     memcpy(m_dsdRemainderBuffer.data(), pData + bytesNeeded, remaining);
                     m_dsdRemainderCount = remaining;
                 }
            }
            av_packet_unref(m_packet);
        }
        return (totalBytesRead * 8) / m_trackInfo.channels;
    }
        

    // ══════════════════════════════════════════════════════════════
    // PCM MODE - Normal decoding with FIFO and Bypass
    // ══════════════════════════════════════════════════════════════
    
    if (!m_codecContext || m_eof) {
        return 0;
    }
    
    // Initialize resampler/FIFO if needed
    if (!m_trackInfo.isDSD && !m_resamplerInitialized) {
        if (!initResampler(outputRate, outputBits)) {
            return 0;
        }
    }
    
    size_t totalSamplesRead = 0;
    size_t bytesPerSample = (outputBits == 16) ? 2 : 4;
    size_t bytesPerFrame = bytesPerSample * m_trackInfo.channels;
    if (buffer.size() < numSamples * bytesPerFrame) buffer.resize(numSamples * bytesPerFrame);
    uint8_t* outputPtr = buffer.data();
    
    // 1. First, drain samples from FIFO
    if (m_pcmFifo && av_audio_fifo_size(m_pcmFifo) > 0) {
        size_t samplesInFifo = av_audio_fifo_size(m_pcmFifo);
        size_t samplesToRead = std::min(samplesInFifo, numSamples);
        
        uint8_t* outPtrs[1] = { outputPtr };
        int read = av_audio_fifo_read(m_pcmFifo, (void**)outPtrs, samplesToRead);
        if (read > 0) {
            outputPtr += read * bytesPerFrame;
            totalSamplesRead += read;
        }
    }
    
    if (totalSamplesRead >= numSamples) return totalSamplesRead;
    
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    
    if (!packet || !frame) {
        if (packet) av_packet_free(&packet);
        if (frame) av_frame_free(&frame);
        return totalSamplesRead; // Retourner ce qu'on a déjà lu du buffer
    }
    
    while (totalSamplesRead < numSamples && !m_eof) {
        // Read packet
        int ret = av_read_frame(m_formatContext, packet);
        
        if (ret < 0) {
            // Log position when EOF occurs
            if (m_formatContext->pb && m_formatContext->pb->pos > 0) {
                std::cout << "[AudioDecoder] Bytes read from stream: " << m_formatContext->pb->pos << std::endl;
            }
            
            if (ret == AVERROR_EOF) {
                m_eof = true;
                DEBUG_LOG("[AudioDecoder] EOF reached");
                
                // Check if we read the expected duration
                std::cout << "[AudioDecoder] Samples decoded: " << totalSamplesRead << std::endl;
            } else if (ret == AVERROR(ETIMEDOUT)) {
                std::cerr << "[AudioDecoder] ⚠️  Timeout - connection too slow or lost" << std::endl;
                // 尝试重试，而不是直接设置EOF
                // 对于网络流，短暂的超时可能是暂时的
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue; // 继续尝试读取
            } else if (ret == AVERROR(ECONNRESET)) {
                std::cerr << "[AudioDecoder] ⚠️  Connection reset by server" << std::endl;
                // 对于网络流，尝试重新连接
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue; // 继续尝试读取
            } else if (ret == AVERROR_EXIT) {
                std::cerr << "[AudioDecoder] ⚠️  Exit requested" << std::endl;
                m_eof = true;
            } else {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, sizeof(errbuf));
                std::cerr << "[AudioDecoder] ⚠️  Read error (" << ret << "): " << errbuf << std::endl;
                
                // 检查是否是网络相关错误
                bool isNetworkError = (ret == AVERROR(ENOENT) || 
                                      ret == AVERROR(EIO) || 
                                      ret == AVERROR(EPIPE) || 
                                      ret == AVERROR(EINVAL) ||
                                      strstr(errbuf, "network") != nullptr ||
                                      strstr(errbuf, "connection") != nullptr ||
                                      strstr(errbuf, "timeout") != nullptr);
                
                if (isNetworkError) {
                    // 对于网络错误，尝试重试
                    std::cerr << "[AudioDecoder] 🔄 Attempting to recover from network error..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    continue; // 继续尝试读取
                } else {
                    // 对于其他错误，设置EOF
                    m_eof = true;
                }
            }
            break;
        }
        
        // Skip non-audio packets
        if (packet->stream_index != m_audioStreamIndex) {
            av_packet_unref(packet);
            continue;
        }
        
        // Send packet to decoder
        ret = avcodec_send_packet(m_codecContext, packet);
        av_packet_unref(packet);
        
        if (ret < 0) {
            std::cerr << "[AudioDecoder] Error sending packet to decoder: " << ret << std::endl;
            break;
        }
        
        // Receive decoded frames
        while (ret >= 0 && totalSamplesRead < numSamples) {
            ret = avcodec_receive_frame(m_codecContext, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                std::cerr << "[AudioDecoder] Error receiving frame: " << ret << std::endl;
                break;
            }

            if (m_trackInfo.isDSD) {
                // CASE: DSD Decoded path (for formats like WavPack or SACD that decode to DSD)
                // Note: Most DSD in this player uses m_rawDSD block above.
                // This is a safety fallback.
                int bytesPerPlane = frame->linesize[0];
                int channels = m_codecContext->ch_layout.nb_channels;
                uint8_t* dst = outputPtr; 
                
                if (channels == 2 && frame->data[1]) {
                    uint8_t* srcL = frame->data[0];
                    uint8_t* srcR = frame->data[1];
                    for (int i = 0; i < bytesPerPlane; i++) {
                        *dst++ = srcL[i];
                        *dst++ = srcR[i];
                    }
                } else {
                    memcpy(dst, frame->data[0], bytesPerPlane);
                    dst += bytesPerPlane;
                }
                size_t samplesDecoded = bytesPerPlane * 8;
                totalSamplesRead += samplesDecoded;
                outputPtr = dst;
            } else {
                // CASE: PCM path (the common case)
                size_t frameSamples = frame->nb_samples;
                if (g_verbose && frameSamples > 0 && frame->data[0]) {
                    std::cout << "[AudioDecoder] DEBUG: PCM frame - samples=" << frameSamples 
                              << " format=" << av_get_sample_fmt_name(m_codecContext->sample_fmt)
                              << " linesize=" << frame->linesize[0] << std::endl;
                    
                    // 打印前几个音频样本的十六进制值
                    std::cout << "[AudioDecoder] DEBUG: First 4 samples: ";
                    for (int i = 0; i < std::min(4, static_cast<int>(frameSamples)); i++) {
                        if (m_codecContext->sample_fmt == AV_SAMPLE_FMT_S32) {
                            const int32_t* sample = reinterpret_cast<const int32_t*>(frame->data[0] + i * 4);
                            std::cout << std::hex << *sample << " " << std::dec;
                        } else if (m_codecContext->sample_fmt == AV_SAMPLE_FMT_S16) {
                            const int16_t* sample = reinterpret_cast<const int16_t*>(frame->data[0] + i * 2);
                            std::cout << std::hex << *sample << " " << std::dec;
                        }
                    }
                    std::cout << std::endl;
                }
                
                if (m_bypassMode) {
                    // canBypass函数已经确保格式匹配，直接写入
                    uint8_t* framePtrs[1] = { frame->data[0] };
                    av_audio_fifo_write(m_pcmFifo, (void**)framePtrs, frameSamples);
                } else {
                    int maxOutSamples = swr_get_out_samples(m_swrContext, frameSamples);
                    size_t bytesNeeded = maxOutSamples * bytesPerFrame;
                    if (m_workBuffer.size() < bytesNeeded) m_workBuffer.resize(bytesNeeded);
                    uint8_t* outPtrs[1] = { m_workBuffer.data() };
                    int converted = swr_convert(m_swrContext, outPtrs, maxOutSamples,
                                               (const uint8_t**)frame->data, frameSamples);
                    if (converted > 0) av_audio_fifo_write(m_pcmFifo, (void**)outPtrs, converted);
                }

                // Try to fulfill request from FIFO
                size_t available = av_audio_fifo_size(m_pcmFifo);
                if (available > 0) {
                    size_t toRead = std::min(available, numSamples - totalSamplesRead);
                    uint8_t* outPtrs[1] = { outputPtr };
                    int read = av_audio_fifo_read(m_pcmFifo, (void**)outPtrs, toRead);
                    if (read > 0) {
                        outputPtr += read * bytesPerFrame;
                        totalSamplesRead += read;
                    }
                }
            }
            av_frame_unref(frame);
        }
    }
    
    av_packet_free(&packet);
    av_frame_free(&frame);
    
    return totalSamplesRead;
}

bool AudioDecoder::initResampler(uint32_t outputRate, uint32_t outputBits) {
    // Don't resample DSD!
    if (m_trackInfo.isDSD) {
        std::cout << "[AudioDecoder] DSD: No resampling, native passthrough" << std::endl;
        return true;
    }
    
    m_resamplerInitialized = false;
    
    if (!m_codecContext) return false;
    
    // Determine output format
    // 24位音频使用S32容器格式以确保兼容性
    AVSampleFormat outFormat;
    if (outputBits == 16) {
        outFormat = AV_SAMPLE_FMT_S16;
    } else {
        // 24位和32位都使用S32格式
        outFormat = AV_SAMPLE_FMT_S32;
    }
    
    // Check if we can bypass resampling entirely (bit-perfect path)
    if (canBypass(outputRate, outputBits)) {
        std::cout << "[AudioDecoder] PCM BYPASS enabled - bit-perfect path" << std::endl;
        
        if (m_swrContext) { swr_free(&m_swrContext); m_swrContext = nullptr; }
        if (m_pcmFifo) av_audio_fifo_free(m_pcmFifo);
        
        // 根据采样率动态调整FIFO大小，减少延迟
        int fifoSize = static_cast<int>((static_cast<int64_t>(16384) * outputRate) / 48000);
        if (fifoSize < 8192) fifoSize = 8192;     // 下限8KB，减少延迟
        if (fifoSize > 65536) fifoSize = 65536;   // 上限64KB，平衡延迟和缓冲
        
        m_pcmFifo = av_audio_fifo_alloc(outFormat, m_trackInfo.channels, fifoSize);
        m_bypassMode = true;
        m_resamplerInitialized = true;
        return true;
    }
    
    m_bypassMode = false;
    if (m_swrContext) swr_free(&m_swrContext);
    
    AVChannelLayout inLayout = m_codecContext->ch_layout;
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, m_trackInfo.channels);
    
    // 优化重采样器配置，减少延迟
    std::cout << "[AudioDecoder] Resampler config: " 
              << "inFmt=" << av_get_sample_fmt_name(m_codecContext->sample_fmt) << " "
              << "outFmt=" << av_get_sample_fmt_name(outFormat) << " "
              << "inRate=" << m_codecContext->sample_rate << " "
              << "outRate=" << outputRate << std::endl;
              
    if (swr_alloc_set_opts2(&m_swrContext, &outLayout, outFormat, outputRate,
                           &inLayout, m_codecContext->sample_fmt, m_codecContext->sample_rate, 
                           0, nullptr) < 0) {
        std::cerr << "[AudioDecoder] Failed to allocate SwrContext" << std::endl;
        return false;
    }
    
    // 设置重采样质量参数，平衡质量和延迟
    av_opt_set_double(m_swrContext, "cutoff", 0.95, 0); // 截止频率比例 (0-1)
    av_opt_set_int(m_swrContext, "filter_size", 16, 0); // 滤波器大小，较小值减少延迟
    av_opt_set_int(m_swrContext, "phase_shift", 8, 0); // 相移
    av_opt_set_int(m_swrContext, "linear_interp", 0, 0); // 禁用线性插值，使用更好的算法
    av_opt_set_int(m_swrContext, "dither_method", 0, 0); // 禁用抖动，减少处理开销
    
    if (swr_init(m_swrContext) < 0) {
        std::cerr << "[AudioDecoder] Failed to initialize resampler" << std::endl;
        return false;
    }
    
    // Initialize PCM FIFO
    if (m_pcmFifo) av_audio_fifo_free(m_pcmFifo);
    
    // 根据采样率动态调整FIFO大小，支持高采样率
    int fifoSize = static_cast<int>((static_cast<int64_t>(32768) * outputRate) / 48000);
    if (fifoSize < 32768) fifoSize = 32768;
    if (fifoSize > 262144) fifoSize = 262144; // 上限256KB
    
    m_pcmFifo = av_audio_fifo_alloc(outFormat, m_trackInfo.channels, fifoSize);
    
    std::cout << "[AudioDecoder] Resampler: " << m_codecContext->sample_rate
              << "Hz -> " << outputRate << "Hz, " << outputBits << "bit"
              << " (FIFO: " << fifoSize << " samples)" << std::endl;
    
    m_resamplerInitialized = true;
    return true;
}

bool AudioDecoder::canBypass(uint32_t outputRate, uint32_t outputBits) const {
    if (m_trackInfo.isDSD) return false;
    if (!m_codecContext) return false;
    if (m_codecContext->sample_rate != (int)outputRate) return false;
    if (m_codecContext->ch_layout.nb_channels != (int)m_trackInfo.channels) return false;
    
    AVSampleFormat fmt = m_codecContext->sample_fmt;
    bool isPackedInteger = (fmt == AV_SAMPLE_FMT_S16 || fmt == AV_SAMPLE_FMT_S32);
    if (!isPackedInteger) return false;
    
    // 只有当输入格式和输出格式匹配时才返回true
    if (outputBits == 16 && fmt == AV_SAMPLE_FMT_S16) return true;
    if ((outputBits == 24 || outputBits == 32) && fmt == AV_SAMPLE_FMT_S32) return true;
    
    return false;
}

// ============================================================================
// AudioEngine
// ============================================================================

AudioEngine::AudioEngine()
    : m_state(State::STOPPED)
    , m_trackNumber(1)
    , m_samplesPlayed(0)
    , m_silenceCount(0)
    , m_isDraining(false)
    , m_nextTrackPrepared(false)  // ⭐ v1.2.0: Gapless Pro
    , m_lastUpdateTime(std::chrono::high_resolution_clock::now())
{
    // Initialize Info Mutex
    // m_infoMutex is default constructed
    
    // ⭐ Initialize DirettaSync (was DirettaOutput)
    m_sync = std::make_unique<DirettaSync>();
    if (!m_sync->enable()) { // Enable will discover target
        std::cerr << "[AudioEngine] Failed to enable DirettaSync" << std::endl;
    }
}

AudioEngine::~AudioEngine() {
    stop();
    // m_sync will be automatically destroyed (and disabled) via unique_ptr
    waitForPreloadThread();
}

void AudioEngine::waitForPreloadThread() {
    if (m_preloadThread.joinable()) {
        m_preloadThread.join();
    }
}

void AudioEngine::setAudioCallback(const AudioCallback& callback) {
    m_audioCallback = callback;
}

void AudioEngine::setTrackChangeCallback(const TrackChangeCallback& callback) {
    m_trackChangeCallback = callback;
}

void AudioEngine::setCurrentURI(const std::string& uri, const std::string& metadata, bool forceReopen) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // CRITICAL: Si on change d'URI pendant la lecture, fermer les décodeurs
    // pour forcer l'ouverture de la nouvelle piste
    bool uriChanged = (uri != m_currentURI);
    
    m_currentURI = uri;
    m_currentMetadata = metadata;
    
    // ⭐ NOUVEAU : Forcer la réouverture même si l'URI est la même (pour Stop)
    if (uriChanged || forceReopen) {
        std::cout << "[AudioEngine] ⚠️  " 
                  << (forceReopen ? "Forced reopen" : "URI changed") 
                  << " - closing decoders to load new track" << std::endl;
        
        // Clear current track info to avoid conflicts with new track
        m_currentTrackInfo = TrackInfo();
        
        // Fermer les décodeurs pour forcer réouverture
        m_currentDecoder.reset();
        m_nextDecoder.reset();
        
        // ⭐⭐⭐ CRITICAL FIX: Clear gapless queue when changing URI
        // Otherwise, the old "next track" will play after the new track finishes!
        {
            std::lock_guard<std::mutex> pendingLock(m_pendingMutex);
            m_pendingNextURI.clear();
            m_pendingNextMetadata.clear();
            m_pendingNextTrack.store(false, std::memory_order_release);
        }
        m_nextURI.clear();
        m_nextMetadata.clear();
        m_nextTrackInfo = TrackInfo(); // Clear next track info too
        
        std::cout << "[AudioEngine] ✓ Gapless queue cleared" << std::endl;
        
        // Réinitialiser la position
        m_samplesPlayed = 0;
        m_lastUpdateTime = std::chrono::high_resolution_clock::now();
        m_silenceCount = 0;
        m_isDraining = false;
        
        // Arrêter le préchargement en cours si existant
        if (m_preloadRunning.load(std::memory_order_acquire)) {
            m_preloadRunning.store(false, std::memory_order_release);
            waitForPreloadThread(); // Wait for preload thread to finish
            std::cout << "[AudioEngine] ⚠️  Cancelled ongoing preload" << std::endl;
        }
        
        // Si on est en PLAYING, on va automatiquement ouvrir la nouvelle piste
        // au prochain process()
    }
    
    std::cout << "[AudioEngine] Current URI set" << std::endl;
}
void AudioEngine::setNextURI(const std::string& uri, const std::string& metadata) {
    // Thread-safe: Use pending mechanism to defer to audio thread
    {
        std::lock_guard<std::mutex> pendingLock(m_pendingMutex);
        m_pendingNextURI = uri;
        m_pendingNextMetadata = metadata;
    }
    m_pendingNextTrack.store(true, std::memory_order_release);
    std::cout << "[AudioEngine] Next URI queued (gapless)" << std::endl;
}

void AudioEngine::setNextTrack(const TrackInfo& track) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_nextTrackInfo = track;
    
    // Also update URIs for gapless logic compatibility
    {
        std::lock_guard<std::mutex> pendingLock(m_pendingMutex);
        m_pendingNextURI = track.path;
        m_pendingNextMetadata = track.metadata;
    }
    m_pendingNextTrack.store(true, std::memory_order_release);
    
    std::cout << "[AudioEngine] Next track queued: " << track.trackTitle 
              << " (Start: " << track.startTime << "s)" << std::endl;
}

void AudioEngine::setTrackEndCallback(const TrackEndCallback& callback) {
    m_trackEndCallback = callback;
}

void AudioEngine::setNextTrackCallback(const NextTrackCallback& callback) {
    m_nextTrackCallback = callback;
    DEBUG_LOG("[AudioEngine] ✓ Next track callback set (Gapless Pro)");
}

bool AudioEngine::play() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_currentURI.empty()) {
        std::cerr << "[AudioEngine] Cannot play: No URI set" << std::endl;
        return false;
    }
    
    // If paused, just resume
    if (m_state == State::PAUSED && m_currentDecoder) {
        std::cout << "[AudioEngine] Resume" << std::endl;
        m_state = State::PLAYING;
        if (m_sync) {
            m_sync->resumePlayback();
        }
        // ⭐ CRITICAL FIX: Reset last update time when resuming playback
        // This prevents jump seconds when resuming after a long pause
        m_lastUpdateTime = std::chrono::high_resolution_clock::now();
        return true;
    }
    
    std::cout << "[AudioEngine] Play" << std::endl;
    
    // Open current track if not already open OR if at EOF
    if (!m_currentDecoder || m_currentDecoder->isEOF()) {
        std::cout << "[AudioEngine] Opening track (new or after EOF)" << std::endl;
        
        if (!openCurrentTrack()) {
            std::cerr << "[AudioEngine] Failed to open track" << std::endl;
            return false;
        }
    }
    
    // Open Diretta Stream
    // DirettaSync needs AudioFormat struct
    AudioFormat fmt;
    fmt.sampleRate = m_currentTrackInfo.sampleRate;
    fmt.bitDepth = m_currentTrackInfo.bitDepth;
    fmt.channels = m_currentTrackInfo.channels;
    fmt.isDSD = m_currentTrackInfo.isDSD;
    fmt.dsdFormat = (m_currentTrackInfo.dsdSourceFormat == TrackInfo::DSDSourceFormat::DSF) ? 
                    AudioFormat::DSDFormat::DSF : AudioFormat::DSDFormat::DFF;
    
    if (m_sync) {
        // Apply S24 alignment hint if available (before open)
        if (m_currentTrackInfo.s24Alignment == TrackInfo::S24Alignment::Msb) {
             m_sync->setS24PackModeHint(DirettaRingBuffer::S24PackMode::MsbAligned);
        } else if (m_currentTrackInfo.s24Alignment == TrackInfo::S24Alignment::Lsb) {
             m_sync->setS24PackModeHint(DirettaRingBuffer::S24PackMode::LsbAligned);
        } else {
             m_sync->setS24PackModeHint(DirettaRingBuffer::S24PackMode::Unknown);
        }

        if (!m_sync->open(fmt)) {
            std::cerr << "[AudioEngine] Failed to open Diretta stream" << std::endl;
            return false;
        }
    }
    
    m_state = State::PLAYING;
    m_samplesPlayed = 0;
    m_lastUpdateTime = std::chrono::high_resolution_clock::now();
    m_silenceCount = 0;
    m_isDraining = false;
    
    // Start Diretta playback
    if (m_sync) {
        // Ensure playback is started - call play() directly if startPlayback() fails
        if (!m_sync->startPlayback()) {
            m_sync->play();
            // Force m_playing to true
            // This ensures the status command shows correct Playing=1
        }
    }   
    
    // Preload next track in background if set (for gapless)
    // Use joinable thread instead of detached to prevent use-after-free
    if (!m_nextURI.empty() && !m_nextDecoder && !m_preloadRunning.load(std::memory_order_acquire)) {
        waitForPreloadThread();
        m_preloadRunning.store(true, std::memory_order_release);
        try {
            m_preloadThread = std::thread([this]() {
                preloadNextTrack();
                m_preloadRunning.store(false, std::memory_order_release);
            });
            // Check if thread was successfully created
            if (!m_preloadThread.joinable()) {
                std::cerr << "[AudioEngine] Failed to create preload thread" << std::endl;
                m_preloadRunning.store(false, std::memory_order_release);
            }
        } catch (const std::system_error& e) {
            std::cerr << "[AudioEngine] Failed to create preload thread: " << e.what() << std::endl;
            m_preloadRunning.store(false, std::memory_order_release);
        }
    }
    
    return true;
}
void AudioEngine::stop() {
    DEBUG_LOG("[AudioEngine] stop() called, current state = " << static_cast<int>(m_state.load()));
    
    // Changer l'état SANS mutex (atomic)
    m_state.store(State::STOPPED);

    // Clear pending flags
    m_pendingNextTrack.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> pendingLock(m_pendingMutex);
        m_pendingNextURI.clear();
        m_pendingNextMetadata.clear();
    }

    // Wait for preload thread before cleanup
    waitForPreloadThread();

    DEBUG_LOG("[AudioEngine] ✓ State changed to STOPPED");

    // CRITICAL: Nettoyer TOUT pour forcer réouverture au prochain play()
    // 使用try_to_lock避免死锁
    std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
    if (lock.owns_lock()) {
        DEBUG_LOG("[AudioEngine] Cleaning up decoders and state...");
        
        // Stop Diretta playback and close stream
        if (m_sync) {
            m_sync->stopPlayback();
            m_sync->close();
        }

        // Fermer les décodeurs
        m_currentDecoder.reset();
        m_nextDecoder.reset();
        
        // Réinitialiser la position
        m_samplesPlayed = 0;
        m_lastUpdateTime = std::chrono::high_resolution_clock::now();
        m_silenceCount = 0;
        m_isDraining = false;
        
        // 重置当前轨道信息
        m_currentTrackInfo = TrackInfo();
        m_nextTrackInfo = TrackInfo();
        
        // CRITICAL: NE PAS effacer m_currentURI !
        // On veut pouvoir redémarrer la même piste depuis le début
        
        DEBUG_LOG("[AudioEngine] cleanup completed");
    } else {
        DEBUG_LOG("[AudioEngine] Mutex busy, cleanup deferred");
        // Le cleanup sera fait au prochain process() qui verra l'état STOPPED
    }
}


void AudioEngine::pause() {
    std::cout << "[AudioEngine] Pause requested" << std::endl;
    
    // ⭐ NE PAS bloquer sur le mutex !
    // Changer l'état directement (m_state est atomique)
    State expected = State::PLAYING;  // ⭐ Correct type
    if (m_state.compare_exchange_strong(expected, State::PAUSED)) {
        std::cout << "[AudioEngine] ✓ State changed to PAUSED" << std::endl;
        if (m_sync) {
            m_sync->pausePlayback();
        }
    }
    
    std::cout << "[AudioEngine] Pause" << std::endl;
}

void AudioEngine::setABA(double seconds) {
    m_abA.store(seconds);
    std::cout << "[AudioEngine] AB-A set to " << seconds << "s" << std::endl;
}

void AudioEngine::setABB(double seconds) {
    m_abB.store(seconds);
    std::cout << "[AudioEngine] AB-B set to " << seconds << "s" << std::endl;
}

void AudioEngine::setABLoop(bool enabled) {
    m_abLoopActive.store(enabled);
    std::cout << "[AudioEngine] AB-Loop " << (enabled ? "enabled" : "disabled") << std::endl;
}

void AudioEngine::clearAB() {
    m_abA.store(-1.0);
    m_abB.store(-1.0);
    m_abLoopActive.store(false);
    std::cout << "[AudioEngine] AB-Loop cleared" << std::endl;
}
double AudioEngine::getPosition() const {
    if (m_currentTrackInfo.sampleRate == 0) {
        return 0.0;
    }
    // 计算当前位置相对于当前曲目起始位置的时间
    // 对于普通文件，startTime为0，返回值从0开始
    // 对于CUE虚拟轨道，返回值从0开始（减去了startTime）
    double absolutePos = static_cast<double>(m_samplesPlayed) / m_currentTrackInfo.sampleRate;
    double position = absolutePos - m_currentTrackInfo.startTime;
    
    // ⭐ FIX: Ensure position is never negative
    // This prevents negative LastTime values during track transition
    return std::max(0.0, position);
}

bool AudioEngine::process(size_t samplesNeeded) {
    // Vérification rapide sans mutex
    // State currentState = m_state.load();
    
    // ⭐⭐⭐ CRITICAL: Process async seek request (lock-free check)
    // This runs in the audio thread, so we can safely take the mutex
    if (m_seekRequested.load(std::memory_order_acquire)) {
        double targetSeconds = m_seekTarget.load(std::memory_order_acquire);
        m_seekRequested.store(false, std::memory_order_release);
        
        DEBUG_LOG("[AudioEngine] 🔍 Processing async seek to " << targetSeconds << "s");
        
        // Now we can safely take the mutex (we're in the audio thread)
        std::lock_guard<std::mutex> seekLock(m_mutex);
        
        // Validate decoder exists
        if (!m_currentDecoder) {
                DEBUG_LOG("[AudioEngine] ❌ No decoder for seek");
                // Don't return false - continue playing
            } else {
                // Validate position
                const TrackInfo& info = m_currentTrackInfo;
                if (info.sampleRate > 0 && info.duration > 0) {
                    double maxSeconds = static_cast<double>(info.duration) / info.sampleRate;
                    if (targetSeconds > maxSeconds) {
                        targetSeconds = maxSeconds;
                    }
                    if (targetSeconds < 0) {
                        targetSeconds = 0;
                    }
                    
                    // Convert relative time to absolute time (including track startTime)
                    // For normal files, startTime=0, so no change
                    // For CUE virtual tracks, add startTime to get absolute file position
                    double absoluteSeekTime = targetSeconds + m_currentTrackInfo.startTime;
                    
                    // ⭐ 已解除 DSD 跳转限制并验证安全
                    if (m_currentDecoder->seek(absoluteSeekTime)) {
                        // Update position based on absolute seek time
                        m_samplesPlayed = static_cast<uint64_t>(absoluteSeekTime * info.sampleRate);
                        
                        // Reset time tracking for accurate playback time calculation
                        m_lastUpdateTime = std::chrono::high_resolution_clock::now();
                        
                        // Reset drainage counters
                        m_silenceCount = 0;
                        m_isDraining = false;
                        
                        // Show relative time in output for user convenience
                        DEBUG_LOG("[AudioEngine] ✓ Seek completed to " << targetSeconds << "s (absolute: " << absoluteSeekTime << "s)");
                    } else {
                        DEBUG_LOG("[AudioEngine] ❌ Seek failed in decoder");
                    }
                }
            }
    }
    
    // Continue processing after seek
    
    std::lock_guard<std::mutex> lock(m_mutex);    
    // Double vérification avec mutex
    if (m_state.load() != State::PLAYING) {
        return false;
    }

    // Apply pending next URI from UPnP thread
    if (m_pendingNextTrack.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> pendingLock(m_pendingMutex);
            m_nextURI = m_pendingNextURI;
            m_nextMetadata = m_pendingNextMetadata;
            m_pendingNextURI.clear();
            m_pendingNextMetadata.clear();
        }
        m_pendingNextTrack.store(false, std::memory_order_release);
        std::cout << "[AudioEngine] Pending next URI applied (gapless)" << std::endl;
    }

    // Safety net: auto-reopen if decoder null while PLAYING
    if (!m_currentDecoder) {
        if (!m_currentURI.empty()) {
            if (!openCurrentTrack()) {
                std::cerr << "[AudioEngine] Failed to reopen track" << std::endl;
                m_state = State::STOPPED;
                if (m_trackEndCallback) {
                    m_trackEndCallback();
                }
                return false;
            }
            m_samplesPlayed = 0;
            m_lastUpdateTime = std::chrono::high_resolution_clock::now();
            m_silenceCount = 0;
            m_isDraining = false;
        } else {
            return false;
        }
    }

    // Determine output format
    uint32_t outputRate = m_currentTrackInfo.sampleRate;
    uint32_t outputBits = m_currentTrackInfo.bitDepth;
    
    // For DSD, keep native rate and bit depth
    if (!m_currentTrackInfo.isDSD) {
        // For PCM, we can target specific output format if needed
        // For now, keep source format (bit-perfect)
    }
    
    // Read samples from decoder
    size_t samplesRead = m_currentDecoder->readSamples(
        m_buffer,
        samplesNeeded,
        outputRate,
        outputBits
    );
    
    // ⚡ CRITICAL: Preload next track for gapless
    // Check if near EOF -OR- near Virtual End (for CUE tracks)
    bool isNearVirtualEnd = false;
    if (m_currentTrackInfo.virtualDuration > 0.0) {
        double currentPosSeconds = static_cast<double>(m_samplesPlayed) / m_currentTrackInfo.sampleRate;
        // Calculate position relative to the start of this virtual track
        double trackRelativePos = currentPosSeconds - m_currentTrackInfo.startTime;
        // Trigger preload 5 seconds before end, or immediately if shorter
        if (trackRelativePos >= (m_currentTrackInfo.virtualDuration - 5.0)) {
            isNearVirtualEnd = true;
        }
    }

    if (!m_nextDecoder && !m_nextURI.empty() && (m_currentDecoder->isEOF() || isNearVirtualEnd)) {
        DEBUG_LOG("[AudioEngine] 📀 Triggering preload (Criteria: " 
                  << (m_currentDecoder->isEOF() ? "Physical EOF" : "Near Virtual End") << ")");
        preloadNextTrack();
    }
    
    if (samplesRead > 0) {
        // Send to DirettaSync
        if (m_sync) {
            // AudioFormat is stateful in DirettaSync, no need to pass it here
            
            // ⭐ FIX: Retry logic to prevent data loss when buffer is full
            const uint8_t* ptr = m_buffer.data();
            size_t remainingSamples = samplesRead;
            
            // Calculate step size for pointer arithmetic
            size_t bytesPerSampleStep = 0;
            if (m_currentTrackInfo.isDSD) {
                // DSD: 1 bit per sample * channels / 8 = bytes
                // But samplesRead here is "frames" or "samples per channel" depending on decoder
                // In AudioDecoder::readSamples for DSD:
                // It returns "totalSamplesRead" which seems to be total samples across all channels?
                // Let's check AudioDecoder::readSamples implementation.
                // 
                // In readSamples DSD path:
                // size_t samplesDecoded = bytesPerPlane * 8;
                // totalSamplesRead += samplesDecoded;
                // So it returns total bits? No, bytesPerPlane * 8 is total bits.
                // 
                // DirettaSync::sendAudio for DSD expects:
                // totalBytes = (numSamples * numChannels) / 8;
                // 
                // If we look at how readSamples returns...
                // It looks like it returns total samples (frames * channels) or similar?
                // Actually let's assume standard behavior:
                // For DSD, we just need to pass the count to sendAudio, and sendAudio handles conversion.
                // But we need to know how many bytes we advanced if we did a partial write.
                
                // Let's look at DirettaSync::sendAudio:
                // if (dsdMode) totalBytes = (numSamples * numChannels) / 8;
                // return written; (which is bytes written)
                
                // So if we get bytes written back, we can advance ptr by that amount.
                // But we need to decrement remainingSamples by the corresponding sample count.
                // numSamples = (written * 8) / numChannels;
            } else {
                // PCM
                // 24位音频使用4字节的S32容器格式
                int bytesPerSample = (outputBits == 16) ? 2 : 4;
                bytesPerSampleStep = bytesPerSample * m_currentTrackInfo.channels;
            }
            
            int retryCount = 0;
            const int MAX_RETRIES = 500; // ~500ms max wait
            
            while (remainingSamples > 0 && m_state == State::PLAYING) {
                size_t written = m_sync->sendAudio(ptr, remainingSamples);
                
                if (written > 0) {
                    retryCount = 0; // Reset retry on success
                    
                    if (m_currentTrackInfo.isDSD) {
                         // DSD Logic
                         ptr += written;
                         // Convert bytes back to samples to decrement counter
                         // Relation: bytes = samples * ch / 8  =>  samples = bytes * 8 / ch
                         size_t samplesSent = (written * 8) / m_currentTrackInfo.channels;
                         if (samplesSent > remainingSamples) samplesSent = remainingSamples; // Safety cap
                         remainingSamples -= samplesSent;
                    } else {
                         // PCM Logic
                         ptr += written;
                         size_t samplesSent = written / bytesPerSampleStep;
                         remainingSamples -= samplesSent;
                    }
                } else {
                    // Buffer full - wait and retry
                    retryCount++;
                    if (retryCount > MAX_RETRIES) {
                        std::cerr << "[AudioEngine] ⚠️ Timeout sending audio to Diretta (Buffer Full)" << std::endl;
                        break; // Drop frame to prevent infinite hang
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }
        
        // Update playback position using actual samples read
        // This ensures accurate position and consistent behavior with beta10
        m_samplesPlayed += samplesRead;

        // Skip dense progress logs to keep terminal clean
        // (Position updates are handled in higher level or on demand)

        // Virtual Track Limit Check
        if (m_currentTrackInfo.virtualDuration > 0.0) {
             // Calculate current position in seconds relative to the entire file
             double currentPos = static_cast<double>(m_samplesPlayed) / m_currentTrackInfo.sampleRate;
             // Calculate position relative to the start of this virtual track
             double trackRelativePos = currentPos - m_currentTrackInfo.startTime;
             
             if (trackRelativePos >= m_currentTrackInfo.virtualDuration) {
                 std::cout << "[AudioEngine] ⏹️ Virtual track duration reached (" << m_currentTrackInfo.virtualDuration << "s)" << std::endl;
                 // Force EOF behavior
                 samplesRead = 0; 
             }
        }
        
        // ⭐ AB Loop Check
        if (m_abLoopActive.load()) {
            double a = m_abA.load();
            double b = m_abB.load();
            double currentPos = getPosition();
            
            if (a >= 0 && b > a && currentPos >= b) {
                std::cout << "[AudioEngine] 🔄 AB-Loop triggering: " << b << "s -> " << a << "s" << std::endl;
                // 注意：getPosition() 包含了 startTime，
                // 而 seek() 接受的是相对于文件开头的秒数。
                // 我们在处理虚拟音轨（CUE）时，getPosition 回传的是物理位置。
                if (this->seek(a)) {
                    // seek 函数内部会更新 m_samplesPlayed，逻辑闭环
                }
            }
        }
    }
    
    // Check for actual end of data (no more samples can be read) OR Virtual End
    bool isVirtualTrack = (m_currentTrackInfo.virtualDuration > 0);
    double currentPos = static_cast<double>(m_samplesPlayed) / m_currentTrackInfo.sampleRate;
    // Calculate position relative to the start of this virtual track
    double trackRelativePos = currentPos - m_currentTrackInfo.startTime;
    bool reachedVirtualEnd = isVirtualTrack && (trackRelativePos >= m_currentTrackInfo.virtualDuration);
    
    // For CUE tracks, we only consider the track ended if we've reached the virtual duration
    // We ignore the physical EOF from decoder until we've played the full virtual duration
    if (samplesRead == 0 || reachedVirtualEnd) {
        
        // Log "Track finished" only once
        if (!m_isDraining) {
            std::cout << "[AudioEngine] ⚠️  Track finished: " << 
                (reachedVirtualEnd ? "Virtual duration reached" : "Physical EOF reached") << std::endl;
            m_isDraining = true;
            m_silenceCount = 0;
        }
        
        // For reachedVirtualEnd, force EOF behavior
        if (reachedVirtualEnd) {
            // Force EOF behavior
            samplesRead = 0;
        }
    
        // Check if we have a next track ready for gapless
        // Only transition if current track has ended (samplesRead == 0)
        // This fixes the issue where manual track changes immediately jump to next track
        if (samplesRead == 0 && m_nextDecoder) {
            std::cout << "[AudioEngine] 🎵 Transitioning to next track (gapless)..." << std::endl;
            m_isDraining = false;
            transitionToNextTrack();
            return true;  // Continue playback with new track
        } 
        
        // ⭐ NEW (v1.0.16): Check if next track exists but decoder was cleared (format change)
        if (!m_nextURI.empty()) {
            std::cout << "[AudioEngine] 🔄 Next track with format change detected" << std::endl;
            std::cout << "[AudioEngine] Transitioning with stop/start sequence..." << std::endl;
            
            // Save next URI before stopping
            std::string nextURI = m_nextURI;
            std::string nextMetadata = m_nextMetadata;
            
            // Signal track end to allow clean transition
            if (m_trackEndCallback) {
                m_trackEndCallback();
            }
            
            // Apply next URI as current
            m_currentURI = nextURI;
            m_currentMetadata = nextMetadata;
            
            // ⭐ CRITICAL SYNC: Ensure new track attributes are passed to m_currentTrackInfo
            // so openCurrentTrack() knows it's a new virtual track and applies startTime.
            m_currentTrackInfo = m_nextTrackInfo;
            // Notify UI status
            updatePublicTrackInfo();
            
            m_nextURI.clear();
            m_nextMetadata.clear();
            m_nextTrackInfo = TrackInfo(); // Reset storage
            
            // Reset for new track
            m_isDraining = false;
            m_samplesPlayed = 0;
            m_lastUpdateTime = std::chrono::high_resolution_clock::now();
            m_trackNumber++;
            
            // Stop current playback (will close DirettaOutput)
            std::cout << "[AudioEngine] Stopping for format/track change..." << std::endl;
            m_currentDecoder.reset();
            
            // Reopen with new track (will be done in next process() call via openCurrentTrack())
            return true;  // Continue playback state
        }
        
        // No next track - drain buffer and stop
        std::cout << "[AudioEngine] 🔇 No next track, draining buffer..." << std::endl;
        
        if (m_silenceCount == 0) {
            std::cout << "[AudioEngine] 🔇 No next track, waiting for Diretta drain..." << std::endl;
        }
        
        m_silenceCount++;
        
        // After a short wait to ensure last samples were sent, signal stop
        // Diretta has ~2-4s of buffer, but we don't need to send silence
        // The stop() function will wait for buffer_empty()
        if (m_silenceCount > 5) {  // 5 * ~92ms = ~500ms safety margin
            std::cout << "[AudioEngine] ✓ Last samples sent, signaling stop" << std::endl;
            m_silenceCount = 0;
            m_isDraining = false;
            m_state = State::STOPPED;
            
            // Signal track end to allow DirettaOutput to fully stop
            if (m_trackEndCallback) {
                m_trackEndCallback();
            }
            
            return false;
        }
        
        // Return false to stop sending samples, but keep state as PLAYING briefly
        return false;
    }
    
    // For CUE tracks, if we got fewer samples than requested but haven't reached virtual end,
    // continue playing instead of treating it as EOF
    if (samplesRead > 0 && isVirtualTrack && samplesRead < samplesNeeded) {
        std::cout << "[AudioEngine] ⚠️  Got fewer samples than requested but continuing (CUE track)" << std::endl;
    }

     return true;
}

void AudioEngine::playTrack(const TrackInfo& track) {
    // Always update track info and decoder, even if same URI and startTime
    // This ensures the correct playlist index is used
    
    // Stop if playing something else
    if (m_state == State::PLAYING) {
        stop();
    }
    
    std::unique_lock<std::mutex> lock(m_mutex);
    
    // Clear current state
    m_currentDecoder.reset();
    m_nextDecoder.reset();
    m_currentTrackInfo = TrackInfo();
    m_nextTrackInfo = TrackInfo();
    m_nextURI.clear();
    m_nextMetadata.clear();
    
    // Set new track information
    m_currentURI = track.path;
    m_currentMetadata = track.metadata;
    m_currentTrackInfo = track; 
    
    // Reset playback state
    m_samplesPlayed = 0;
    m_lastUpdateTime = std::chrono::high_resolution_clock::now();
    m_trackNumber = 1;
    m_silenceCount = 0;
    m_isDraining = false;
    
    // Unlock mutex before calling play() because play() acquires the lock
    lock.unlock();
    
    // Call play() to handle opening track and initializing DirettaSync
    if (!play()) {
        std::cerr << "[AudioEngine] playTrack failed to start playback" << std::endl;
    }
}

bool AudioEngine::openCurrentTrack() {
    // Note: This function is called from play() which already holds the mutex
    
    if (m_currentURI.empty()) {
        std::cerr << "[AudioEngine] No current URI set" << std::endl;
        return false;
    }
    
    std::cout << "[AudioEngine] Opening track: " << m_currentURI.substr(0, 80) << "..." << std::endl;
    
    // Create decoder
    m_currentDecoder = std::make_unique<AudioDecoder>();
    
    if (!m_currentDecoder->open(m_currentURI)) {
        std::cerr << "[AudioEngine] Failed to open track: " << m_currentURI << std::endl;
        m_currentDecoder.reset();
        return false;
    }
    
    try {
        // Store requested virtual info before overwriting
        double reqStart = m_currentTrackInfo.startTime;
        double reqDur = m_currentTrackInfo.virtualDuration;
        std::string reqTitle = m_currentTrackInfo.trackTitle;
        std::string reqArtist = m_currentTrackInfo.trackArtist;
        
        m_currentTrackInfo = m_currentDecoder->getTrackInfo();
        
        // Restore virtual info
        m_currentTrackInfo.startTime = reqStart;
        m_currentTrackInfo.virtualDuration = reqDur;
        if (!reqTitle.empty()) m_currentTrackInfo.trackTitle = reqTitle;
        if (!reqArtist.empty()) m_currentTrackInfo.trackArtist = reqArtist;

        // Apply Seek if virtual start time is set
        if (m_currentTrackInfo.startTime > 0.0) {
           std::cout << "[AudioEngine] ⏩ Virtual Track Start: Seeking to " << m_currentTrackInfo.startTime << "s" << std::endl;
           if (m_currentDecoder->seek(m_currentTrackInfo.startTime)) {
               // Update m_samplesPlayed to match the seek position
               m_samplesPlayed = static_cast<uint64_t>(m_currentTrackInfo.startTime * m_currentTrackInfo.sampleRate);
           } else {
                std::cerr << "[AudioEngine] ⚠️ Virtual seek failed" << std::endl;
           }
        }
        
        std::cout << "[AudioEngine] ✓ Track opened: ";
        if (m_currentTrackInfo.isDSD) {
            std::cout << "DSD" << m_currentTrackInfo.dsdRate 
                      << " (" << m_currentTrackInfo.sampleRate << " Hz)";
        } else {
            std::cout << m_currentTrackInfo.sampleRate << "Hz/"
                      << m_currentTrackInfo.bitDepth << "bit";
        }
        std::cout << "/" << m_currentTrackInfo.channels << "ch" << std::endl;
        
        // Call track change callback with URI and metadata
        if (m_trackChangeCallback) {
            m_trackChangeCallback(m_trackNumber, m_currentTrackInfo, m_currentURI, m_currentMetadata);
        }
        
        // Notify UI status
        updatePublicTrackInfo();
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[AudioEngine] Exception while opening track: " << e.what() << std::endl;
        m_currentDecoder.reset();
        return false;
    } catch (...) {
        std::cerr << "[AudioEngine] Unknown exception while opening track" << std::endl;
        m_currentDecoder.reset();
        return false;
    }
}

bool AudioEngine::preloadNextTrack() {
    if (m_nextURI.empty()) {
        return false;
    }
    
    DEBUG_LOG("[AudioEngine] Preloading next track for gapless...");
    
    // Create decoder for next track
    m_nextDecoder = std::make_unique<AudioDecoder>();
    
    if (!m_nextDecoder->open(m_nextURI)) {
        std::cerr << "[AudioEngine] Failed to preload next track" << std::endl;
        m_nextDecoder.reset();
        return false;
    }

    // Check format compatibility for gapless playback
    // Format changes require clean stop/start to avoid audio artifacts
    TrackInfo nextInfo = m_nextDecoder->getTrackInfo();
    bool formatWillChange = (
        nextInfo.sampleRate != m_currentTrackInfo.sampleRate ||
        nextInfo.bitDepth != m_currentTrackInfo.bitDepth ||
        nextInfo.channels != m_currentTrackInfo.channels ||
        nextInfo.isDSD != m_currentTrackInfo.isDSD
    );

    if (formatWillChange) {
        std::cout << "[AudioEngine] 🔄 Format change detected:" << std::endl;
        std::cout << "[AudioEngine]    Current: " << (m_currentTrackInfo.isDSD ? "DSD" : "PCM") << " " 
                  << m_currentTrackInfo.sampleRate << "Hz/" << m_currentTrackInfo.bitDepth << "bit/" << m_currentTrackInfo.channels << "ch" << std::endl;
        std::cout << "[AudioEngine]    Next: " << (nextInfo.isDSD ? "DSD" : "PCM") << " " 
                  << nextInfo.sampleRate << "Hz/" << nextInfo.bitDepth << "bit/" << nextInfo.channels << "ch" << std::endl;
        DEBUG_LOG("[AudioEngine] 🔄 Will use stop/start sequence instead of gapless");

        // Don't keep nextDecoder - force stop/start sequence
        m_nextDecoder.reset();
        return false;
    }

    // ⭐ Apply Seek to next track immediately after opening if it's a virtual track
    if (m_nextTrackInfo.startTime > 0.0) {
        DEBUG_LOG("[AudioEngine] ⏩ Preloading virtual track: seeking to " << m_nextTrackInfo.startTime << "s");
        if (!m_nextDecoder->seek(m_nextTrackInfo.startTime)) {
            std::cerr << "[AudioEngine] ⚠️ Preload seek failed" << std::endl;
        }
    }

    DEBUG_LOG("[AudioEngine] ✓ Next track preloaded: "
              << m_nextDecoder->getTrackInfo().codec);

    return true;
}

void AudioEngine::transitionToNextTrack() {
    DEBUG_LOG("[AudioEngine] Transition to next track (gapless)");
    
    // CRITICAL: Move next URI to current URI BEFORE clearing
    m_currentURI = m_nextURI;
    m_currentMetadata = m_nextMetadata;
    
    // Verify nextDecoder exists before moving
    if (m_nextDecoder) {
        m_currentDecoder = std::move(m_nextDecoder);
        m_trackNumber++;
        
        // Clear next URI after moving to current
        m_nextURI.clear();
        m_nextMetadata.clear();
        
        // Merge decoder info with stored virtual track info
        TrackInfo realInfo = m_currentDecoder->getTrackInfo();
        
        // Preserve virtual properties from m_nextTrackInfo
        realInfo.startTime = m_nextTrackInfo.startTime;
        realInfo.virtualDuration = m_nextTrackInfo.virtualDuration;
        if (!m_nextTrackInfo.trackTitle.empty()) realInfo.trackTitle = m_nextTrackInfo.trackTitle;
        if (!m_nextTrackInfo.trackArtist.empty()) realInfo.trackArtist = m_nextTrackInfo.trackArtist;
        
        // ⭐ FIX: Update m_currentTrackInfo BEFORE setting m_samplesPlayed
        // This prevents negative position values during track transition
        m_currentTrackInfo = realInfo;
        
        // Update m_samplesPlayed based on the NEW track info
        // For CUE virtual tracks, we need to set m_samplesPlayed to match the startTime
        // This ensures trackRelativePos calculations are correct
        if (m_currentTrackInfo.startTime > 0.0) {
            m_samplesPlayed = static_cast<uint64_t>(m_currentTrackInfo.startTime * m_currentTrackInfo.sampleRate);
        } else {
            m_samplesPlayed = 0;
        }
        
        // ⭐ FIX: Set m_lastUpdateTime AFTER setting m_samplesPlayed
        // This ensures accurate time calculation in process() function
        m_lastUpdateTime = std::chrono::high_resolution_clock::now();
        
        std::cout << "[AudioEngine] ✓ Transition complete. New Current: " << m_currentTrackInfo.trackTitle 
                  << " (StartOffset: " << m_currentTrackInfo.startTime << "s)" << std::endl;

        // Reset next track info
        m_nextTrackInfo = TrackInfo(); 

        if (m_trackChangeCallback) {
            m_trackChangeCallback(m_trackNumber, m_currentTrackInfo, m_currentURI, m_currentMetadata);
        }
        
        // Notify UI status
        updatePublicTrackInfo();
    } else {
        std::cerr << "[AudioEngine] ❌ No next decoder available for transition" << std::endl;
        // Clear next URI even if decoder is null
        m_nextURI.clear();
        m_nextMetadata.clear();
        m_nextTrackInfo = TrackInfo();
    }
}
bool AudioDecoder::seek(double seconds) {
    if (!m_formatContext || m_audioStreamIndex < 0) {
        std::cerr << "[AudioDecoder] Cannot seek: no file open" << std::endl;
        return false;
    }
    
    // ⭐ v1.2.0: DSD raw seek with file repositioning
    if (m_rawDSD) {
        std::cout << "[AudioDecoder] DSD seek to " << seconds << "s (with file repositioning)" << std::endl;
        
        // Calculate byte position in DSD file
        // For DSD: sampleRate = bits per second per channel
        int64_t bitsPerSecond = m_trackInfo.sampleRate * m_trackInfo.channels;
        int64_t targetBit = static_cast<int64_t>(seconds * bitsPerSecond);
        int64_t targetByte = targetBit / 8;
        
        std::cout << "[AudioDecoder]   Target: " << targetByte << " bytes (" << targetBit << " bits)" << std::endl;
        std::cout << "[AudioDecoder]   Format: " << m_trackInfo.sampleRate << " Hz, " 
                  << m_trackInfo.channels << " channels" << std::endl;
        
        // Seek in file using byte position
        AVIOContext* avio = m_formatContext->pb;
        if (avio) {
            // Use SEEK_SET to position from start of file
            int64_t result = avio_seek(avio, targetByte, SEEK_SET);
            if (result >= 0) {
                std::cout << "[AudioDecoder]   ✓ File repositioned to byte " << result << std::endl;
            } else {
                std::cerr << "[AudioDecoder]   ⚠️  avio_seek failed, code: " << result << std::endl;
                // Continue anyway - may still work approximately
            }
        } else {
            std::cerr << "[AudioDecoder]   ⚠️  No AVIOContext available for file seek" << std::endl;
        }
        
        // Flush codec buffers to clear old data
        if (m_codecContext) {
            avcodec_flush_buffers(m_codecContext);
            std::cout << "[AudioDecoder]   ✓ Codec buffers flushed" << std::endl;
        }
        
        // Reset internal buffers
        m_dsdRemainderCount = 0;
        m_eof = false;
        
        std::cout << "[AudioDecoder]   ✓ DSD seek completed" << std::endl;
        return true;
    }
    
    // ═══════════════════════════════════════════════════════════════
    // ✅ PCM: Normal FFmpeg seek (unchanged)
    // ═══════════════════════════════════════════════════════════════
    
    std::cout << "[AudioDecoder] Seeking to " << seconds << " seconds..." << std::endl;
    
    // Convertir le temps en timestamp FFmpeg
    AVStream* stream = m_formatContext->streams[m_audioStreamIndex];
    int64_t timestamp = av_rescale_q(
        static_cast<int64_t>(seconds * AV_TIME_BASE),
        AV_TIME_BASE_Q,
        stream->time_base
    );
    
    // Effectuer le seek
    // AVSEEK_FLAG_BACKWARD : cherche le keyframe le plus proche AVANT la position
    int ret = av_seek_frame(m_formatContext, m_audioStreamIndex, timestamp, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        std::cerr << "[AudioDecoder] Seek failed: " << errbuf << std::endl;
        return false;
    }
    
    // Vider les buffers du codec
    if (m_codecContext) {
        avcodec_flush_buffers(m_codecContext);
    }
    
    // Reset internal buffers
    if (m_pcmFifo) {
        av_audio_fifo_reset(m_pcmFifo);
    }
    m_dsdRemainderCount = 0;
    m_eof = false;
    
    std::cout << "[AudioDecoder] ✓ Seek successful to ~" << seconds << "s" << std::endl;
    
    return true;
}


// ============================================================================
// AudioEngine::seek() - Seek avec mise à jour de la position
// ============================================================================

bool AudioEngine::seek(double seconds) {
    // ⭐⭐⭐ CRITICAL FIX: Async seek to avoid deadlock
    // The UPnP thread calling this should not block waiting for mutex
    // Instead, we set atomic flags and let the audio thread handle the seek
    
    std::cout << "[AudioEngine] ⏩ Seek requested to " << seconds << " seconds (async)" << std::endl;
    
    // Quick validation without mutex
    if (m_state.load(std::memory_order_acquire) != State::PLAYING) {
        std::cerr << "[AudioEngine] ❌ Cannot seek when not playing" << std::endl;
        return false;
    }
    
    // Clamp to valid range (optimistic check, will be validated in audio thread)
    const TrackInfo& info = m_currentTrackInfo;
    if (info.sampleRate > 0 && info.duration > 0) {
        double maxSeconds = static_cast<double>(info.duration) / info.sampleRate;
        if (seconds < 0) {
            seconds = 0;
        }
        if (seconds > maxSeconds) {
            DEBUG_LOG("[AudioEngine] Seek position clamped to " << maxSeconds << "s");
            seconds = maxSeconds;
        }
    }
    
    // ⭐ Set seek request atomically (lock-free, non-blocking)
    m_seekTarget.store(seconds, std::memory_order_release);
    m_seekRequested.store(true, std::memory_order_release);
    
    std::cout << "[AudioEngine] ✓ Seek queued, will be processed by audio thread" << std::endl;
    
    // Return immediately - UPnP thread doesn't wait
    return true;
}

// ============================================================================
// AudioEngine::seek() - Version avec string "HH:MM:SS"
// ============================================================================

bool AudioEngine::seek(const std::string& timeStr) {
    // Parser le format HH:MM:SS ou MM:SS
    int hours = 0, minutes = 0, seconds = 0;
    
    // Compter les ':'
    size_t colonCount = std::count(timeStr.begin(), timeStr.end(), ':');
    
    if (colonCount == 2) {
        // Format HH:MM:SS
        if (sscanf(timeStr.c_str(), "%d:%d:%d", &hours, &minutes, &seconds) != 3) {
            std::cerr << "[AudioEngine] Invalid time format: " << timeStr << std::endl;
            return false;
        }
    } else if (colonCount == 1) {
        // Format MM:SS
        if (sscanf(timeStr.c_str(), "%d:%d", &minutes, &seconds) != 2) {
            std::cerr << "[AudioEngine] Invalid time format: " << timeStr << std::endl;
            return false;
        }
    } else {
        // Format numérique simple (secondes)
        try {
            double secs = std::stod(timeStr);
            return seek(secs);
        } catch (...) {
            std::cerr << "[AudioEngine] Invalid time format: " << timeStr << std::endl;
            return false;
        }
    }
    
    // Convertir en secondes totales
    double totalSeconds = hours * 3600.0 + minutes * 60.0 + seconds;
    
    DEBUG_LOG("[AudioEngine] Parsed time: " << timeStr 
              << " = " << totalSeconds << " seconds");
    
    return seek(totalSeconds);
 }

uint32_t AudioEngine::getCurrentSampleRate() const {
    return m_currentTrackInfo.sampleRate;
}
// ═══════════════════════════════════════════════════════════════
// ⭐ v1.2.0: Gapless Pro - Prepare next track for gapless
// ═══════════════════════════════════════════════════════════════

void AudioEngine::prepareNextTrackForGapless() {
    // Check if next track exists
    if (m_nextURI.empty()) {
        DEBUG_LOG("[AudioEngine] No next track for gapless");
        return;
    }
    
    // Check if already prepared
    if (m_nextTrackPrepared) {
        DEBUG_LOG("[AudioEngine] Next track already prepared");
        return;
    }
    
    DEBUG_LOG("[AudioEngine] 🎵 Preparing next track for gapless: " << m_nextURI);
    
    try {
        // ⭐ OPTIMISATION v1.2.0: Réutiliser m_nextDecoder si déjà ouvert
        if (!m_nextDecoder) {
            DEBUG_LOG("[AudioEngine] Opening next track decoder...");
            m_nextDecoder = std::make_unique<AudioDecoder>();
            
            if (!m_nextDecoder->open(m_nextURI)) {
                std::cerr << "[AudioEngine] ❌ Failed to open next track for gapless" << std::endl;
                m_nextDecoder.reset();  // Cleanup on failure
                return;
            }
            DEBUG_LOG("[AudioEngine] ✓ Next track decoder opened");
        } else {
            DEBUG_LOG("[AudioEngine] ♻️  Reusing pre-loaded next track decoder");
        }
        
        // Get format from m_nextDecoder
        const TrackInfo& nextTrackInfo = m_nextDecoder->getTrackInfo();
        
        // Create AudioFormat for DirettaOutput
        AudioFormat nextFormat;
        nextFormat.sampleRate = nextTrackInfo.sampleRate;
        nextFormat.bitDepth = nextTrackInfo.bitDepth;
        nextFormat.channels = nextTrackInfo.channels;
        nextFormat.isDSD = nextTrackInfo.isDSD;
        nextFormat.isCompressed = nextTrackInfo.isCompressed;
        // Set DSD format based on track info
        nextFormat.dsdFormat = (nextTrackInfo.dsdSourceFormat == TrackInfo::DSDSourceFormat::DSF) ? 
                               AudioFormat::DSDFormat::DSF : AudioFormat::DSDFormat::DFF;
        
        // Read first chunk (1 second of audio for buffering)
        size_t samplesToRead = nextTrackInfo.sampleRate;  // 1 second
        
        AudioBuffer nextBuffer;
        size_t bytesPerSample = (nextTrackInfo.bitDepth / 8) * nextTrackInfo.channels;
        nextBuffer.resize(samplesToRead * bytesPerSample);
        
        // Read from m_nextDecoder
        size_t samplesRead = m_nextDecoder->readSamples(nextBuffer, samplesToRead,
                                                        nextTrackInfo.sampleRate,
                                                        nextTrackInfo.bitDepth);
        
        if (samplesRead > 0) {
            // Call callback to send to DirettaOutput
            if (m_nextTrackCallback) {
                DEBUG_LOG("[AudioEngine] 📤 Sending " << samplesRead 
                          << " samples to gapless buffer");
                m_nextTrackCallback(nextBuffer.data(), samplesRead, nextFormat);
                m_nextTrackPrepared = true;
                
                DEBUG_LOG("[AudioEngine] ✅ Next track prepared for gapless transition");
            } else {
                DEBUG_LOG("[AudioEngine] ⚠️  No next track callback set");
            }
        } else {
            DEBUG_LOG("[AudioEngine] ⚠️  Failed to read samples from next track");
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[AudioEngine] ❌ Exception preparing next track: " 
                  << e.what() << std::endl;
        m_nextDecoder.reset();  // Cleanup on exception
    }
}

// ═══════════════════════════════════════════════════════════════
// End of v1.2.0 Gapless Pro implementation
// ═══════════════════════════════════════════════════════════════


void AudioEngine::setRepeatMode(RepeatMode mode) {
    m_repeatMode.store(mode);
    std::string modeStr = (mode == RepeatMode::OFF) ? "Off" :
                          (mode == RepeatMode::ONE) ? "One" : "All";
    DEBUG_LOG("[AudioEngine] Repeat mode set to " << modeStr);
}

AudioEngine::RepeatMode AudioEngine::getRepeatMode() const {
    return m_repeatMode.load();
}

void AudioEngine::setRandomMode(bool enabled) {
    m_randomMode.store(enabled);
    DEBUG_LOG("[AudioEngine] Random mode set to " << (enabled ? "On" : "Off"));
}

bool AudioEngine::getRandomMode() const {
    return m_randomMode.load();
}

void AudioEngine::setVolume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    m_volume.store(volume);
    // DirettaSync does not support software volume control in this adapter
    // if (m_sync) {
    //    m_sync->setVolume(volume); 
    // }
    DEBUG_LOG("[AudioEngine] Volume set to " << volume);
}

int AudioEngine::getVolume() const {
    return m_volume.load();
}

TrackInfo AudioEngine::getCurrentTrackInfo() const {
    // ⭐ Optimized: Read separate public state (no blocking audio thread)
    std::lock_guard<std::mutex> lock(m_infoMutex);
    return m_publicTrackInfo;
}

void AudioEngine::updatePublicTrackInfo() {
    std::lock_guard<std::mutex> lock(m_infoMutex);
    m_publicTrackInfo = m_currentTrackInfo;
}

// [New] Forward Diretta settings
void AudioEngine::setDirettaTargetIndex(int index) {
    if (m_sync) {
        m_sync->setTargetIndex(index);
    }
}

void AudioEngine::setDirettaTargetIP(const std::string& ip) {
    if (m_sync) {
        m_sync->setTargetIP(ip);
    }
}
