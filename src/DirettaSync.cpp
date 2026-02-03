/**
 * @file DirettaSync.cpp
 * @brief Unified Diretta sync implementation
 *
 * Based on MPD Diretta Output Plugin v0.4.0
 * Preserves DSD planar handling from original UPnP renderer
 */

#include "DirettaSync.h"
#include <stdexcept>
#include <iomanip>
#include <pthread.h>
#include <sched.h>

namespace {

// G1: Interruptible wait helper for format transitions
// Uses condition variable instead of sleep_for to allow shutdown interruption
// Returns true if wait completed, false if interrupted by wakeup signal
bool interruptibleWait(std::mutex& mutex, std::condition_variable& cv,
                       std::atomic<bool>& wakeupFlag, int timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex);
    bool interrupted = cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                   [&wakeupFlag]() { return wakeupFlag.load(std::memory_order_acquire); });
    if (interrupted) {
        wakeupFlag.store(false, std::memory_order_release);  // Reset for next use
    }
    return !interrupted;  // Return true if timeout (normal), false if interrupted
}

// F1: Worker thread priority elevation for reduced jitter
// Sets SCHED_FIFO real-time priority (requires root on Linux)
// Returns true on success, false on failure (logs warning but continues)
bool setRealtimePriority(int priority = 50) {
    struct sched_param param;
    param.sched_priority = priority;

    int ret = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (ret != 0) {
        // Not fatal - may not have CAP_SYS_NICE or running as non-root
        if (g_verbose) {
            std::cerr << "[DirettaSync] Warning: Could not set SCHED_FIFO priority "
                      << priority << " (error " << ret << ")" << std::endl;
        }
        return false;
    }

    if (g_verbose) {
        std::cout << "[DirettaSync] Worker thread set to SCHED_FIFO priority " << priority << std::endl;
    }
    return true;
}

class RingAccessGuard {
public:
    RingAccessGuard(std::atomic<int>& users, const std::atomic<bool>& reconfiguring)
        : users_(users), active_(false) {
        if (reconfiguring.load(std::memory_order_acquire)) {
            return;
        }
        // C2: acquire ensures increment visible to beginReconfigure() before ring ops
        users_.fetch_add(1, std::memory_order_acquire);
        if (reconfiguring.load(std::memory_order_acquire)) {
            // C2: bail-out - never entered guarded section, relaxed is safe
            users_.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
        active_ = true;
    }

    ~RingAccessGuard() {
        if (active_) {
            // C2: release ensures all ring ops complete before decrement
            users_.fetch_sub(1, std::memory_order_release);
        }
    }

    bool active() const { return active_; }

private:
    std::atomic<int>& users_;
    bool active_;
};
} // namespace

//=============================================================================
// Constructor / Destructor
//=============================================================================

// extern bool g_verbose; // Already in header
// bool g_verbose = true; // CAUSES LINKER ERROR

DirettaSync::DirettaSync() {
    m_ringBuffer.resize(44100 * 2 * 4, 0x00);
    DIRETTA_LOG("Created");
}

DirettaSync::~DirettaSync() {
    disable();
    DIRETTA_LOG("Destroyed");
}

//=============================================================================
// Initialization (Enable/Disable like MPD)
//=============================================================================

bool DirettaSync::enable(const DirettaConfig& config) {
    if (m_enabled) {
        DIRETTA_LOG("Already enabled");
        return true;
    }

    m_config = config;
    DIRETTA_LOG("Enabling...");

    if (!discoverTarget()) {
        DIRETTA_LOG("Failed to discover target");
        return false;
    }

    if (!measureMTU()) {
        DIRETTA_LOG("MTU measurement failed, using fallback");
    }

    m_calculator = std::make_unique<DirettaCycleCalculator>(m_effectiveMTU);

    if (!openSyncConnection()) {
        DIRETTA_LOG("Failed to open sync connection");
        return false;
    }

    m_enabled = true;
    DIRETTA_LOG("Enabled, MTU=" << m_effectiveMTU);
    return true;
}

void DirettaSync::disable() {
    DIRETTA_LOG("Disabling...");

    // G1: Signal any pending format transition waits to wake up immediately
    {
        std::lock_guard<std::mutex> lock(m_transitionMutex);
        m_transitionWakeup.store(true, std::memory_order_release);
    }
    m_transitionCv.notify_all();

    if (m_open) {
        close();
    }

    if (m_enabled) {
        shutdownWorker();
        DIRETTA::Sync::close();
        m_sdkOpen = false;
        m_calculator.reset();
        m_enabled = false;
    }

    m_hasPreviousFormat = false;
    DIRETTA_LOG("Disabled");
}

bool DirettaSync::openSyncConnection() {
    ACQUA::Clock cycleTime = ACQUA::Clock::MicroSeconds(m_config.cycleTime);

    DIRETTA_LOG("Opening DIRETTA::Sync with threadMode=" << m_config.threadMode);

    bool opened = false;
    for (int attempt = 0; attempt < DirettaRetry::OPEN_RETRIES && !opened; attempt++) {
        if (attempt > 0) {
            DIRETTA_LOG("open() retry #" << attempt);
            std::this_thread::sleep_for(std::chrono::milliseconds(DirettaRetry::OPEN_DELAY_MS));
        }
        opened = DIRETTA::Sync::open(
            DIRETTA::Sync::THRED_MODE(m_config.threadMode),
            cycleTime, 0, "DirettaRenderer", 0x44525400,
            -1, -1, 0, DIRETTA::Sync::MSMODE_MS3);
    }

    if (!opened) {
        DIRETTA_LOG("DIRETTA::Sync::open failed after 3 attempts");
        return false;
    }

    m_sdkOpen = true;
    inquirySupportFormat(m_targetAddress);

    if (g_verbose) {
        logSinkCapabilities();
    }

    return true;
}

//=============================================================================
// Target Discovery
//=============================================================================

bool DirettaSync::discoverTarget() {
    DIRETTA_LOG("Discovering Diretta target...");

    DIRETTA::Find::Setting findSettings;
    findSettings.Loopback = false;
    findSettings.ProductID = 0;
    // 移除可能限制搜索范围的参数
    // findSettings.Name = "DirettaRenderer";
    // findSettings.MyID = 0x44525400;

    DIRETTA::Find find(findSettings);
    if (!find.open()) {
        DIRETTA_LOG("Failed to open finder");
        return false;
    }

    DIRETTA::Find::PortResalts results;
    if (!find.findOutput(results) || results.empty()) {
        find.close();
        DIRETTA_LOG("No Diretta targets found");
        return false;
    }

    DIRETTA_LOG("Found " << results.size() << " target(s)");

    bool selected = false;
    if (!m_targetIPString.empty()) {
        for (const auto& target : results) {
            if (target.first.get_str() == m_targetIPString) {
                m_targetAddress = target.first;
                DIRETTA_LOG("Selected by IP: " << target.second.targetName << " (" << m_targetIPString << ")");
                selected = true;
                break;
            }
        }
        if (!selected) {
            DIRETTA_LOG("Requested target IP " << m_targetIPString << " not found, falling back to index logic");
        }
    }

    if (!selected) {
        if (results.size() == 1 || m_targetIndex == 0) {
            auto it = results.begin();
            m_targetAddress = it->first;
            DIRETTA_LOG("Selected: " << it->second.targetName);
        } else if (m_targetIndex > 0 && m_targetIndex < static_cast<int>(results.size())) {
            auto it = results.begin();
            std::advance(it, m_targetIndex);
            m_targetAddress = it->first;
            DIRETTA_LOG("Selected target #" << (m_targetIndex + 1));
        } else {
            auto it = results.begin();
            m_targetAddress = it->first;
            DIRETTA_LOG("Selected first target: " << it->second.targetName);
        }
    }

    find.close();
    return true;
}

bool DirettaSync::measureMTU() {
    if (m_mtuOverride > 0) {
        m_effectiveMTU = m_mtuOverride;
        DIRETTA_LOG("Using configured MTU=" << m_effectiveMTU);
        return true;
    }

    if (m_config.mtu > 0) {
        m_effectiveMTU = m_config.mtu;
        DIRETTA_LOG("Using config MTU=" << m_effectiveMTU);
        return true;
    }

    DIRETTA_LOG("Measuring MTU...");

    DIRETTA::Find::Setting findSettings;
    findSettings.Loopback = false;
    findSettings.ProductID = 0;

    DIRETTA::Find find(findSettings);
    if (!find.open()) {
        m_effectiveMTU = m_config.mtuFallback;
        return false;
    }

    uint32_t measuredMTU = 0;
    bool ok = find.measSendMTU(m_targetAddress, measuredMTU);
    find.close();

    if (ok && measuredMTU > 0) {
        m_effectiveMTU = measuredMTU;
        DIRETTA_LOG("Measured MTU=" << m_effectiveMTU);
        return true;
    }

    m_effectiveMTU = m_config.mtuFallback;
    DIRETTA_LOG("MTU measurement failed, using fallback=" << m_effectiveMTU);
    return false;
}

bool DirettaSync::verifyTargetAvailable() {
    DIRETTA::Find::Setting findSettings;
    findSettings.Loopback = false;
    findSettings.ProductID = 0;

    DIRETTA::Find find(findSettings);
    if (!find.open()) return false;

    DIRETTA::Find::PortResalts results;
    bool found = find.findOutput(results) && !results.empty();
    find.close();

    return found;
}

void DirettaSync::listTargets() {
    DIRETTA::Find::Setting findSettings;
    findSettings.Loopback = false;
    findSettings.ProductID = 0;

    DIRETTA::Find find(findSettings);
    if (!find.open()) {
        std::cerr << "Failed to open Diretta finder" << std::endl;
        return;
    }

    DIRETTA::Find::PortResalts results;
    if (!find.findOutput(results) || results.empty()) {
        std::cout << "No Diretta targets found" << std::endl;
        find.close();
        return;
    }

    std::cout << "\nAvailable Diretta Targets (" << results.size() << " found):\n" << std::endl;

    int index = 1;
    for (const auto& target : results) {
        const auto& info = target.second;
        std::cout << "[" << index << "] " << info.targetName << std::endl;

        // Show output/port name if available (differentiates I2S vs USB, etc.)
        if (!info.outputName.empty()) {
            std::cout << "    Output: " << info.outputName << std::endl;
        }

        // Show port numbers
        std::cout << "    Port: IN=" << info.PI << " OUT=" << info.PO;
        if (info.multiport) {
            std::cout << " (multiport)";
        }
        std::cout << std::endl;

        // Show configuration URL if available
        if (!info.config.empty()) {
            std::cout << "    Config: " << info.config << std::endl;
        }

        // Show SDK version
        std::cout << "    Version: " << info.version << std::endl;

        // Show Product ID
        std::cout << "    ProductID: 0x" << std::hex << info.productID << std::dec << std::endl;

        std::cout << std::endl;
        index++;
    }

    find.close();
}

std::string DirettaSync::getTargetsJson() {
    DIRETTA::Find::Setting findSettings;
    findSettings.Loopback = false;
    findSettings.ProductID = 0;

    DIRETTA::Find find(findSettings);
    if (!find.open()) {
        return "[]";
    }

    DIRETTA::Find::PortResalts results;
    if (!find.findOutput(results) || results.empty()) {
        find.close();
        return "[]";
    }

    std::stringstream ss;
    ss << "[";
    
    bool first = true;
    int index = 1;
    for (const auto& target : results) {
        if (!first) ss << ",";
        first = false;
        
        const auto& info = target.second;
        ss << "{";
        ss << "\"index\":" << index << ","; 
        ss << "\"name\":\"" << info.targetName << "\",";
        // SDK doesn't expose MAC directly in TargetConnectInfo. 
        // Use ProductID + IP as unique ID or just leave blank/mock if not critical.
        // Or use target.first (IPAddress) string as ID.
        ss << "\"mac\":\"" << info.TargetAddress.get_str() << "\","; 
        ss << "\"ip\":\"" << info.TargetAddress.get_str() << "\",";
        ss << "\"model\":\"" << info.targetName << "\","; 
        ss << "\"version\":\"" << info.version << "\",";
        ss << "\"connected\":" << (false ? "true" : "false"); 
        ss << "}";
        index++;
    }
    
    ss << "]";
    find.close();
    return ss.str();
}

void DirettaSync::logSinkCapabilities() {
    const auto& info = getSinkInfo();
    std::cout << "[DirettaSync] Sink capabilities:" << std::endl;
    std::cout << "[DirettaSync]   PCM: " << (info.checkSinkSupportPCM() ? "YES" : "NO") << std::endl;
    std::cout << "[DirettaSync]   DSD: " << (info.checkSinkSupportDSD() ? "YES" : "NO") << std::endl;
    std::cout << "[DirettaSync]   DSD LSB: " << (info.checkSinkSupportDSDlsb() ? "YES" : "NO") << std::endl;
    std::cout << "[DirettaSync]   DSD MSB: " << (info.checkSinkSupportDSDmsb() ? "YES" : "NO") << std::endl;

    // SDK 148: Log supported multi-stream modes
    // supportMSmode is a bitmask: bit0=MS1, bit1=MS2, bit2=MS3
    uint16_t msmode = info.supportMSmode;
    std::cout << "[DirettaSync]   MS modes: "
              << ((msmode & 0x01) ? "MS1 " : "")
              << ((msmode & 0x02) ? "MS2 " : "")
              << ((msmode & 0x04) ? "MS3 " : "")
              << (msmode == 0 ? "(none)" : "")
              << std::endl;

    // Warn if MS3 (our default) is not supported
    if (!(msmode & 0x04) && msmode != 0) {
        std::cerr << "[DirettaSync] WARNING: Target does not support MS3 mode (using MS3 anyway)" << std::endl;
    }
}

//=============================================================================
// Open/Close (Connection Management)
//=============================================================================

bool DirettaSync::open(const AudioFormat& format) {

    std::cout << "[DirettaSync] ========== OPEN ==========" << std::endl;
    std::cout << "[DirettaSync] Format: " << format.sampleRate << "Hz/"
              << format.bitDepth << "bit/" << format.channels << "ch "
              << (format.isDSD ? "DSD" : "PCM") << std::endl;
    DIRETTA_LOG("Opening audio format: " << format.sampleRate << "Hz/"
                << format.bitDepth << "bit/" << format.channels << "ch "
                << (format.isDSD ? "DSD" : "PCM"));

    if (!m_enabled) {
        std::cout << "[DirettaSync] Not enabled, attempting lazy discovery..." << std::endl;
        // Try to enable again (using stored config which was set on first attempt)
        if (!enable(m_config)) {
             std::cerr << "[DirettaSync] ERROR: Auto-enable failed (Target not found?)" << std::endl;
             return false;
        }
    }

    // Reopen SDK if it was released (e.g., after playlist end)
    if (!m_sdkOpen) {
        std::cout << "[DirettaSync] SDK was released, reopening..." << std::endl;
        if (!openSyncConnection()) {
            std::cerr << "[DirettaSync] ERROR: Failed to reopen SDK" << std::endl;
            return false;
        }
        std::cout << "[DirettaSync] SDK reopened successfully" << std::endl;
    }

    bool newIsDsd = format.isDSD;
    bool needFullConnect = true;  // Whether we need connectPrepare/connect/connectWait

    // Fast path: Already open with same format - just reset buffer and resume
    // This avoids the expensive setSink/connect sequence for same-format track transitions
    if (m_open && m_hasPreviousFormat) {
        bool sameFormat = (m_previousFormat.sampleRate == format.sampleRate &&
                          m_previousFormat.bitDepth == format.bitDepth &&
                          m_previousFormat.channels == format.channels &&
                          m_previousFormat.isDSD == format.isDSD);

        std::cout << "[DirettaSync]   Previous: " << m_previousFormat.sampleRate << "Hz/"
                  << m_previousFormat.bitDepth << "bit/" << m_previousFormat.channels << "ch"
                  << (m_previousFormat.isDSD ? " DSD" : " PCM") << std::endl;
        std::cout << "[DirettaSync]   Current:  " << format.sampleRate << "Hz/"
                  << format.bitDepth << "bit/" << format.channels << "ch"
                  << (format.isDSD ? " DSD" : " PCM") << std::endl;

        if (sameFormat) {
            std::cout << "[DirettaSync] Same format - quick resume (no setSink)" << std::endl;

            // Send silence before transition to flush Diretta pipeline
            if (m_isDsdMode.load(std::memory_order_acquire)) {
                requestShutdownSilence(30);
                auto start = std::chrono::steady_clock::now();
                while (m_silenceBuffersRemaining.load(std::memory_order_acquire) > 0) {
                    if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(100)) break;
                    std::this_thread::yield();
                }
            }

            // Clear buffer and reset flags
            // NOTE: Do NOT reset m_postOnlineDelayDone for quick resume!
            // The DAC is already stable from the previous track - no need
            // to send additional silence after prefill completes.
            m_ringBuffer.clear();
            m_prefillComplete = false;
            // m_postOnlineDelayDone stays true - DAC already stable
            m_stabilizationCount = 0;
            m_stopRequested = false;
            m_draining = false;
            m_silenceBuffersRemaining = 0;
            play();
            m_playing = true;
            m_paused = false;
            std::cout << "[DirettaSync] ========== OPEN COMPLETE (quick) ==========" << std::endl;
            return true;
        } else {
            // Format change detected
            bool wasDSD = m_previousFormat.isDSD;
            bool nowDSD = format.isDSD;
            bool nowPCM = !format.isDSD;

            // Detect rate changes (DSD or PCM)
            // DSD512×44.1 (22,579,200 Hz) ↔ DSD512×48 (24,576,000 Hz) requires clock domain change
            bool isDsdRateChange = wasDSD && nowDSD &&
                                   (m_previousFormat.sampleRate != format.sampleRate);
            bool isPcmRateChange = !wasDSD && nowPCM &&
                                   (m_previousFormat.sampleRate != format.sampleRate);

            if (wasDSD && (nowPCM || isDsdRateChange)) {
                // DSD→PCM or any DSD rate change: Full close/reopen for clean transition
                // I2S targets are timing-sensitive and need a clean break
                // Rate changes cause noise if target's internal buffers aren't fully flushed
                // Clock domain changes (44.1kHz ↔ 48kHz family) also require full reset
                // Note: We can't send silence here because playback is already stopped
                // (auto-stop happens before URI change), so getNewStream() isn't being called
                if (nowPCM) {
                    std::cout << "[DirettaSync] DSD->PCM transition - full close/reopen" << std::endl;
                } else {
                    int prevMultiplier = m_previousFormat.sampleRate / 2822400;
                    int newMultiplier = format.sampleRate / 2822400;
                    std::cout << "[DirettaSync] DSD" << (prevMultiplier * 64) << "->DSD"
                              << (newMultiplier * 64) << " rate change - full close/reopen" << std::endl;
                }

                int dsdMultiplier = m_previousFormat.sampleRate / 2822400;  // DSD64=1, DSD512=8
                std::cout << "[DirettaSync] Previous format was DSD" << (dsdMultiplier * 64) << std::endl;

                // Clear any pending silence requests (playback is stopped, can't send anyway)
                m_silenceBuffersRemaining = 0;

                // Stop playback and disconnect
                stop();
                disconnect(true);

                // CRITICAL: Stop worker thread BEFORE closing SDK to prevent use-after-free
                m_running = false;
                {
                    std::lock_guard<std::mutex> lock(m_workerMutex);
                    if (m_workerThread.joinable()) {
                        m_workerThread.join();
                    }
                }

                // Now safe to close SDK - worker thread is stopped
                DIRETTA::Sync::close();

                m_open = false;
                m_playing = false;
                m_paused = false;

                // Extended delay for target to fully reset
                // DSD→PCM needs delay for clock domain switch
                // DSD rate downgrade needs time to flush internal buffers
                // G4: Scale delay with DSD rate - higher rates have deeper pipelines
                // G1: Use interruptible wait for responsive shutdown
                int resetDelayMs = 200 * std::max(1, dsdMultiplier);  // 200ms (DSD64) to 1600ms (DSD512)
                std::cout << "[DirettaSync] Waiting " << resetDelayMs
                          << "ms for target to reset..." << std::endl;
                interruptibleWait(m_transitionMutex, m_transitionCv, m_transitionWakeup, resetDelayMs);

                // Reopen DIRETTA::Sync fresh
                ACQUA::Clock cycleTime = ACQUA::Clock::MicroSeconds(m_config.cycleTime);
                if (!DIRETTA::Sync::open(
                        DIRETTA::Sync::THRED_MODE(m_config.threadMode),
                        cycleTime, 0, "DirettaRenderer", 0x44525400,
                        -1, -1, 0, DIRETTA::Sync::MSMODE_MS3)) {
                    std::cerr << "[DirettaSync] Failed to re-open DIRETTA::Sync" << std::endl;
                    return false;
                }
                std::cout << "[DirettaSync] DIRETTA::Sync reopened" << std::endl;

                // Fall through to full open path (needFullConnect is already true)
            } else if (isPcmRateChange) {
                // PCM rate change: Full close/reopen for clean transition
                // Same issue as DSD - stale samples at old rate cause transition noise
                std::cout << "[DirettaSync] PCM " << m_previousFormat.sampleRate << "Hz->"
                          << format.sampleRate << "Hz rate change - full close/reopen" << std::endl;

                // Clear any pending silence requests
                m_silenceBuffersRemaining = 0;

                // Stop playback and disconnect
                stop();
                disconnect(true);

                // CRITICAL: Stop worker thread BEFORE closing SDK to prevent use-after-free
                m_running = false;
                {
                    std::lock_guard<std::mutex> lock(m_workerMutex);
                    if (m_workerThread.joinable()) {
                        m_workerThread.join();
                    }
                }

                // Now safe to close SDK - worker thread is stopped
                DIRETTA::Sync::close();

                m_open = false;
                m_playing = false;
                m_paused = false;

                // Shorter delay for PCM rate change (TEST: reduced from 200 to 100)
                // G1: Use interruptible wait for responsive shutdown
                int resetDelayMs = 100;
                std::cout << "[DirettaSync] Waiting " << resetDelayMs
                          << "ms for target to reset..." << std::endl;
                interruptibleWait(m_transitionMutex, m_transitionCv, m_transitionWakeup, resetDelayMs);

                // Reopen DIRETTA::Sync fresh
                ACQUA::Clock cycleTime = ACQUA::Clock::MicroSeconds(m_config.cycleTime);
                if (!DIRETTA::Sync::open(
                        DIRETTA::Sync::THRED_MODE(m_config.threadMode),
                        cycleTime, 0, "DirettaRenderer", 0x44525400,
                        -1, -1, 0, DIRETTA::Sync::MSMODE_MS3)) {
                    std::cerr << "[DirettaSync] Failed to re-open DIRETTA::Sync" << std::endl;
                    return false;
                }
                std::cout << "[DirettaSync] DIRETTA::Sync reopened" << std::endl;

                // Fall through to full open path
            } else {
                // Other format changes (PCM→DSD, bit depth change):
                // use existing reopenForFormatChange()
                std::cout << "[DirettaSync] Format change - reopen" << std::endl;
                if (!reopenForFormatChange()) {
                    std::cerr << "[DirettaSync] Failed to reopen for format change" << std::endl;
                    return false;
                }
            }
            needFullConnect = true;
        }
    }

    // Full reset for first open or after format change reopen
    if (needFullConnect) {
        fullReset();
    }
    m_isDsdMode.store(newIsDsd, std::memory_order_release);

    uint32_t effectiveSampleRate;
    int effectiveChannels = format.channels;
    int bitsPerSample;

    if (m_isDsdMode.load(std::memory_order_acquire)) {
        uint32_t dsdBitRate = format.sampleRate;
        uint32_t byteRate = dsdBitRate / 8;
        effectiveSampleRate = dsdBitRate;
        bitsPerSample = 1;

        DIRETTA_LOG("DSD: bitRate=" << dsdBitRate << " byteRate=" << byteRate);

        configureSinkDSD(dsdBitRate, format.channels, format);
        configureRingDSD(byteRate, format.channels);
    } else {
        effectiveSampleRate = format.sampleRate;

        int acceptedBits;
        configureSinkPCM(format.sampleRate, format.channels, format.bitDepth, acceptedBits);
        bitsPerSample = acceptedBits;

        int direttaBps = (acceptedBits == 32) ? 4 : (acceptedBits == 24) ? 3 : 2;
        int inputBps = (format.bitDepth == 32 || format.bitDepth == 24) ? 4 : 2;

        configureRingPCM(format.sampleRate, format.channels, direttaBps, inputBps, format.bitDepth);
    }

    unsigned int cycleTimeUs = calculateCycleTime(effectiveSampleRate, effectiveChannels, bitsPerSample);
    ACQUA::Clock cycleTime = ACQUA::Clock::MicroSeconds(cycleTimeUs);

    // Initial delay - Target needs time to prepare for new format
    // Longer delay for first open/reconnect, shorter for reconfigure
    int initialDelayMs = needFullConnect ? 500 : 200;
    std::this_thread::sleep_for(std::chrono::milliseconds(initialDelayMs));

    // setSink reconfiguration
    bool sinkSet = false;
    int maxAttempts = needFullConnect ? DirettaRetry::SETSINK_RETRIES_FULL : DirettaRetry::SETSINK_RETRIES_QUICK;
    int retryDelayMs = needFullConnect ? DirettaRetry::SETSINK_DELAY_FULL_MS : DirettaRetry::SETSINK_DELAY_QUICK_MS;
    for (int attempt = 0; attempt < maxAttempts && !sinkSet; attempt++) {
        if (attempt > 0) {
            DIRETTA_LOG("setSink retry #" << attempt);
            std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
        }
        sinkSet = setSink(m_targetAddress, cycleTime, false, m_effectiveMTU);
    }

    if (!sinkSet) {
        std::cerr << "[DirettaSync] Failed to set sink after " << maxAttempts << " attempts" << std::endl;
        return false;
    }

    // SDK 148: Query format support after setSink to initialize internal structures
    // This may be required for stream objects to be properly allocated
    if (needFullConnect) {
        inquirySupportFormat(m_targetAddress);
    }

    applyTransferMode(m_config.transferMode, cycleTime);

    // Connect sequence - only needed after disconnect
    if (needFullConnect) {
        if (!connectPrepare()) {
            std::cerr << "[DirettaSync] connectPrepare failed" << std::endl;
            return false;
        }

        bool connected = false;
        for (int attempt = 0; attempt < DirettaRetry::CONNECT_RETRIES && !connected; attempt++) {
            if (attempt > 0) {
                DIRETTA_LOG("connect retry #" << attempt);
                std::this_thread::sleep_for(std::chrono::milliseconds(DirettaRetry::CONNECT_DELAY_MS));
            }
            connected = connect(0);
        }

        if (!connected) {
            std::cerr << "[DirettaSync] connect failed" << std::endl;
            return false;
        }

        if (!connectWait()) {
            std::cerr << "[DirettaSync] connectWait failed" << std::endl;
            disconnect();
            return false;
        }
    } else {
        DIRETTA_LOG("Skipping connect sequence (still connected)");
    }

    // Clear buffer and start playback
    m_ringBuffer.clear();
    m_prefillComplete = false;
    m_postOnlineDelayDone = false;

    play();

    if (!waitForOnline(m_config.onlineWaitMs)) {
        DIRETTA_LOG("WARNING: Did not come online within timeout");
    }

    m_postOnlineDelayDone = false;
    m_stabilizationCount = 0;

    // Save format state
    m_previousFormat = format;
    m_hasPreviousFormat = true;
    m_currentFormat = format;

    // Start the sync worker thread to send audio data
    startSyncWorker();
    
    m_open = true;
    m_playing = true;
    m_paused = false;

    std::cout << "[DirettaSync] ========== OPEN COMPLETE ==========" << std::endl;
    return true;
}

void DirettaSync::close() {
    std::cout << "[DirettaSync] Close()" << std::endl;

    if (!m_open) {
        DIRETTA_LOG("Not open");
        return;
    }

    // Request shutdown silence
    requestShutdownSilence(m_isDsdMode.load(std::memory_order_acquire) ? 50 : 20);

    auto start = std::chrono::steady_clock::now();
    while (m_silenceBuffersRemaining.load(std::memory_order_acquire) > 0) {
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(150)) {
            DIRETTA_LOG("Silence timeout");
            break;
        }
        std::this_thread::yield();
    }

    m_stopRequested = true;

    stop();
    disconnect(true);  // Wait for proper disconnection before returning

    // SDK 148: Close SDK completely to allow clean reopen on next track
    DIRETTA::Sync::close();
    m_sdkOpen = false;

    // Brief delay for target to process disconnect (like reopenForFormatChange)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Stop worker thread
    m_running = false;
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        if (m_workerThread.joinable()) {
            m_workerThread.join();
        }
    }

    m_open = false;
    m_playing = false;
    m_paused = false;

    // Reset cached consumer generation to force reload on next getNewStream()
    m_cachedConsumerGen = UINT32_MAX;

    DIRETTA_LOG("Close() done (SDK closed)");
}

void DirettaSync::release() {
    std::cout << "[DirettaSync] Release() - fully releasing target" << std::endl;

    // First do a normal close if still open
    if (m_open) {
        close();
    }

    // Now fully close the SDK connection so target is released
    if (m_sdkOpen) {
        DIRETTA_LOG("Closing SDK connection...");

        // Shutdown worker thread
        m_running = false;
        {
            std::lock_guard<std::mutex> lock(m_workerMutex);
            if (m_workerThread.joinable()) {
                m_workerThread.join();
            }
        }

        // Close SDK-level connection
        DIRETTA::Sync::close();
        m_sdkOpen = false;

        // Brief delay to ensure target processes the disconnect
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::cout << "[DirettaSync] Target released" << std::endl;
    }

    // Clear format state so next open() starts fresh
    m_hasPreviousFormat = false;

    // v2.0.1 FIX: Reset cached consumer generation to force reload on next getNewStream()
    // Without this, if m_consumerStateGen wraps around to match m_cachedConsumerGen,
    // stale cached values could be used after SDK reopen
    m_cachedConsumerGen = UINT32_MAX;  // Force mismatch on next getNewStream()
}

bool DirettaSync::reopenForFormatChange() {
    DIRETTA_LOG("reopenForFormatChange: stopping...");

    stop();
    disconnect(true);

    // CRITICAL: Stop worker thread BEFORE closing SDK to prevent use-after-free
    // The worker thread calls getNewStream() which accesses SDK structures
    m_running = false;
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        if (m_workerThread.joinable()) {
            m_workerThread.join();
        }
    }

    // Now safe to close SDK - worker thread is stopped
    DIRETTA::Sync::close();

    // G1: Use interruptible wait for responsive shutdown
    DIRETTA_LOG("Waiting " << m_config.formatSwitchDelayMs << "ms...");
    interruptibleWait(m_transitionMutex, m_transitionCv, m_transitionWakeup,
                      static_cast<int>(m_config.formatSwitchDelayMs));

    ACQUA::Clock cycleTime = ACQUA::Clock::MicroSeconds(m_config.cycleTime);

    if (!DIRETTA::Sync::open(
            DIRETTA::Sync::THRED_MODE(m_config.threadMode),
            cycleTime, 0, "DirettaRenderer", 0x44525400,
            -1, -1, 0, DIRETTA::Sync::MSMODE_MS3)) {
        std::cerr << "[DirettaSync] Failed to re-open sync" << std::endl;
        return false;
    }

    // NOTE: Do NOT call setSink() or inquirySupportFormat() here!
    // The caller will handle all configuration with the proper cycleTime
    // calculated from the new format. Calling setSink() twice with different
    // parameters corrupts SDK 148's internal stream state.

    DIRETTA_LOG("reopenForFormatChange complete (SDK reopened, awaiting caller config)");
    return true;
}

void DirettaSync::fullReset() {
    DIRETTA_LOG("fullReset()");

    m_stopRequested = true;
    m_draining = false;

    int waitCount = 0;
    while (m_workerActive.load(std::memory_order_acquire) && waitCount < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waitCount++;
    }

    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        ReconfigureGuard guard(*this);

        m_prefillComplete = false;
        m_postOnlineDelayDone = false;
        m_silenceBuffersRemaining = 0;
        m_stabilizationCount = 0;
        m_streamCount = 0;
        m_pushCount = 0;
        m_isDsdMode.store(false, std::memory_order_release);
        m_needDsdBitReversal.store(false, std::memory_order_release);
        m_needDsdByteSwap.store(false, std::memory_order_release);
        m_isLowBitrate.store(false, std::memory_order_release);
        m_need24BitPack.store(false, std::memory_order_release);
        m_need16To32Upsample.store(false, std::memory_order_release);
        m_need16To24Upsample.store(false, std::memory_order_release);
        m_bytesPerFrame.store(0, std::memory_order_release);
        m_framesPerBufferRemainder.store(0, std::memory_order_release);
        m_framesPerBufferAccumulator.store(0, std::memory_order_release);

        m_ringBuffer.clear();
    }

    // v2.0.1 FIX: Reset cached consumer generation to force reload on next getNewStream()
    m_cachedConsumerGen = UINT32_MAX;

    m_stopRequested = false;
}

//=============================================================================
// Sink Configuration
//=============================================================================

void DirettaSync::configureSinkPCM(int rate, int channels, int inputBits, int& acceptedBits) {
    std::lock_guard<std::mutex> lock(m_configMutex);

    DIRETTA_LOG("Configuring sink PCM: " << rate << "Hz " << channels << "ch "
                << "inputBits=" << inputBits);

    DIRETTA::FormatConfigure fmt;
    fmt.setSpeed(rate);
    fmt.setChannel(channels);

    if (inputBits == 24) {
        // 优先尝试 24-bit (Pack24, 3字节传输)
        // 这能完美匹配 FFmpeg 的输出，解决音量低的问题
        fmt.setFormat(DIRETTA::FormatID::FMT_PCM_SIGNED_24);
        if (checkSinkSupport(fmt)) {
            setSinkConfigure(fmt);
            acceptedBits = 24;
            DIRETTA_LOG("Sink PCM configured: " << rate << "Hz " << channels << "ch "
                        << acceptedBits << "-bit (优先24位模式，解决音量问题)");
            return;
        }
        // 失败才回退到 32-bit
        DIRETTA_LOG("24-bit 模式不支持，回退到 32-bit");
    }

    fmt.setFormat(DIRETTA::FormatID::FMT_PCM_SIGNED_32);
    if (checkSinkSupport(fmt)) {
        setSinkConfigure(fmt);
        acceptedBits = 32;
        DIRETTA_LOG("Sink PCM configured: " << rate << "Hz " << channels << "ch "
                    << acceptedBits << "-bit");
        return;
    }

    fmt.setFormat(DIRETTA::FormatID::FMT_PCM_SIGNED_16);
    if (checkSinkSupport(fmt)) {
        setSinkConfigure(fmt);
        acceptedBits = 16;
        DIRETTA_LOG("Sink PCM configured: " << rate << "Hz " << channels << "ch "
                    << acceptedBits << "-bit");
        return;
    }

    throw std::runtime_error("No supported PCM format found");
}

void DirettaSync::configureSinkDSD(uint32_t dsdBitRate, int channels, const AudioFormat& format) {
    std::lock_guard<std::mutex> lock(m_configMutex);

    DIRETTA_LOG("DSD: bitRate=" << dsdBitRate << " ch=" << channels);

    // ⭐ IMPORTANT: AudioEngine now ALWAYS outputs LSB format!
    // DFF files are bit-reversed at AudioEngine layer, so we receive LSB regardless of source.
    DIRETTA_LOG("Original DSD format: " << (format.dsdFormat == AudioFormat::DSDFormat::DSF ? "DSF" : "DFF"));
    DIRETTA_LOG("AudioEngine output: LSB (normalized)");

    const auto& info = getSinkInfo();
    DIRETTA_LOG("Sink DSD support: " << (info.checkSinkSupportDSD() ? "YES" : "NO"));

    DIRETTA::FormatConfigure fmt;
    fmt.setSpeed(dsdBitRate);
    fmt.setChannel(channels);

    // ⭐ CALCULATE RATE FLAGS (Critical for Sink Support)
    DIRETTA::FormatID rateFlags = static_cast<DIRETTA::FormatID>(0);
    
    // Base 44.1kHz multiples
    if (dsdBitRate % 44100 == 0) {
        rateFlags = DIRETTA::FormatID::RAT_44100;
        int ratio = dsdBitRate / 44100;
        switch (ratio) {
            case 64:  rateFlags |= DIRETTA::FormatID::RAT_MP64; break;
            case 128: rateFlags |= DIRETTA::FormatID::RAT_MP128; break;
            case 256: rateFlags |= DIRETTA::FormatID::RAT_MP256; break;
            case 512: rateFlags |= DIRETTA::FormatID::RAT_MP512; break;
            default:  DIRETTA_LOG("Warning: Unknown DSD ratio: " << ratio); break;
        }
    } else if (dsdBitRate % 48000 == 0) {
        // Base 48kHz multiples (rare for DSD but possible)
        rateFlags = DIRETTA::FormatID::RAT_48000;
        int ratio = dsdBitRate / 48000;
        switch (ratio) {
            case 64:  rateFlags |= DIRETTA::FormatID::RAT_MP64; break;
            case 128: rateFlags |= DIRETTA::FormatID::RAT_MP128; break;
            case 256: rateFlags |= DIRETTA::FormatID::RAT_MP256; break;
            case 512: rateFlags |= DIRETTA::FormatID::RAT_MP512; break;
            default:  DIRETTA_LOG("Warning: Unknown DSD ratio: " << ratio); break;
        }
    }

    // ⭐ BETA 10 STRATEGY: Force LSB|LITTLE format (no negotiation)
    // AudioEngine outputs LSB (DFF already bit-reversed), so we tell sink it's LSB|LITTLE
    // No additional conversion needed - direct passthrough
    fmt.setFormat(DIRETTA::FormatID::FMT_DSD1 |
                  DIRETTA::FormatID::FMT_DSD_SIZ_32 |
                  DIRETTA::FormatID::FMT_DSD_LSB |
                  DIRETTA::FormatID::FMT_DSD_LITTLE |
                  rateFlags);
    
    setSinkConfigure(fmt);
    
    // ⭐ CRITICAL: AudioEngine 已完成所有 DSD 格式转换
    // - DFF 文件已经过比特反转（MSB→LSB）
    // - 数据已转换为交错4字节块格式 [4B L][4B R]...
    // DirettaRingBuffer 使用 Passthrough 模式，不再进行任何转换
    m_needDsdBitReversal.store(false, std::memory_order_release);
    m_needDsdByteSwap.store(false, std::memory_order_release);
    m_dsdConversionMode.store(DirettaRingBuffer::DSDConversionMode::Passthrough, std::memory_order_release);
    
    DIRETTA_LOG("Sink DSD: LSB | LITTLE (Forced Beta 10 Style) | Swap=OFF Reverse=OFF | Passthrough Mode");
}

//=============================================================================
// Ring Buffer Configuration
//=============================================================================

void DirettaSync::configureRingPCM(int rate, int channels, int direttaBps, int inputBps, int bitDepth) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    ReconfigureGuard guard(*this);

    m_sampleRate.store(rate, std::memory_order_release);
    m_channels.store(channels, std::memory_order_release);
    m_bytesPerSample.store(direttaBps, std::memory_order_release);
    m_inputBytesPerSample.store(inputBps, std::memory_order_release);
    m_need24BitPack.store(direttaBps == 3 && inputBps == 4, std::memory_order_release);
    // 启用32位shift处理
    // 当输入是S32 (4字节) 且实际有效位深是24bit时，开启shift
    // 1. 将24位数据从低位移到高位，解决"声音小"
    // 2. 自动修正符号位，解决"严重杂音"
    bool needShift = (direttaBps == 4 && inputBps == 4 && bitDepth == 24);
    m_need32BitShift.store(needShift, std::memory_order_release);
    if (needShift) {
        DIRETTA_LOG("Need 32-bit shift: true (24位音频需要移位处理)");
    } else {
        DIRETTA_LOG("Need 32-bit shift: false (不需要移位处理)");
    }
    m_need16To32Upsample.store(direttaBps == 4 && inputBps == 2, std::memory_order_release);
    m_need16To24Upsample.store(direttaBps == 3 && inputBps == 2, std::memory_order_release);
    m_isDsdMode.store(false, std::memory_order_release);
    m_needDsdBitReversal.store(false, std::memory_order_release);
    m_needDsdByteSwap.store(false, std::memory_order_release);
    m_isLowBitrate.store(direttaBps <= 2 && rate <= 48000, std::memory_order_release);
    // PCM 模式下 DSD 转换标志无意义，设为 Passthrough
    m_dsdConversionMode.store(DirettaRingBuffer::DSDConversionMode::Passthrough, std::memory_order_release);

    // Increment format generation to invalidate cached values in sendAudio
    m_formatGeneration.fetch_add(1, std::memory_order_release);
    // C1: Also increment consumer generation for getNewStream
    m_consumerStateGen.fetch_add(1, std::memory_order_release);

    size_t bytesPerSecond = static_cast<size_t>(rate) * channels * direttaBps;
    size_t ringSize = DirettaBuffer::calculateBufferSize(bytesPerSecond, DirettaBuffer::PCM_BUFFER_SECONDS);

    m_ringBuffer.resize(ringSize, 0x00);
    ringSize = m_ringBuffer.size();

    int bytesPerFrame = channels * direttaBps;

    // Calculate bytesPerBuffer to match DirettaCycleCalculator
    // The cycle time is calculated as: cycleTimeUs = (efficientMTU / bytesPerSecond) * 1000000
    // SDK's m_effectiveMTU already accounts for IP/UDP headers, only Diretta overhead (~3 bytes)
    // Tested by Hoorna: OVERHEAD=3 works at MTU 1500
    constexpr int OVERHEAD = 3;
    int efficientMTU = static_cast<int>(m_effectiveMTU) - OVERHEAD;
    if (efficientMTU < 64) efficientMTU = 1497;  // Fallback (1500 - 3)

    // Align to frame boundary for clean audio
    int framesPerBuffer = efficientMTU / bytesPerFrame;
    int bytesPerBuffer = framesPerBuffer * bytesPerFrame;

    // For low sample rates (<=96kHz), 1ms worth of data fits in MTU, use that instead
    // This gives better timing resolution and matches original behavior
    int bytesPerMs = (rate / 1000) * bytesPerFrame;
    if (bytesPerMs <= efficientMTU) {
        // Low sample rate: use 1ms buffers with drift correction for 44.1kHz family
        int framesBase = rate / 1000;
        int framesRemainder = rate % 1000;
        bytesPerBuffer = framesBase * bytesPerFrame;

        m_bytesPerFrame.store(bytesPerFrame, std::memory_order_release);
        m_framesPerBufferRemainder.store(static_cast<uint32_t>(framesRemainder), std::memory_order_release);
        m_framesPerBufferAccumulator.store(0, std::memory_order_release);
        m_bytesPerBuffer.store(bytesPerBuffer, std::memory_order_release);

        DIRETTA_LOG("PCM buffer (1ms): " << bytesPerBuffer << " bytes (" << framesBase << " frames)");
    } else {
        // High sample rate: use MTU-sized buffers, no drift correction needed
        // Cycle time and buffer size are matched via DirettaCycleCalculator
        m_bytesPerFrame.store(bytesPerFrame, std::memory_order_release);
        m_framesPerBufferRemainder.store(0, std::memory_order_release);
        m_framesPerBufferAccumulator.store(0, std::memory_order_release);
        m_bytesPerBuffer.store(bytesPerBuffer, std::memory_order_release);

        DIRETTA_LOG("PCM buffer (MTU): " << bytesPerBuffer << " bytes (" << framesPerBuffer << " frames)");
    }

    m_prefillTarget = DirettaBuffer::calculatePrefill(bytesPerSecond, false,
        m_isLowBitrate.load(std::memory_order_acquire));
    m_prefillTarget = std::min(m_prefillTarget, ringSize / 4);
    m_prefillComplete = false;

    DIRETTA_LOG("Ring PCM: " << rate << "Hz " << channels << "ch "
                << direttaBps << "bps, buffer=" << ringSize
                << ", bytesPerBuffer=" << bytesPerBuffer
                << ", prefill=" << m_prefillTarget
                << ", bitDepth=" << bitDepth
                << ", needShift=" << m_need32BitShift.load(std::memory_order_acquire));
}

void DirettaSync::configureRingDSD(uint32_t byteRate, int channels) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    ReconfigureGuard guard(*this);

    m_isDsdMode.store(true, std::memory_order_release);
    m_need24BitPack.store(false, std::memory_order_release);
    m_need16To32Upsample.store(false, std::memory_order_release);
    m_need16To24Upsample.store(false, std::memory_order_release);
    m_channels.store(channels, std::memory_order_release);
    m_isLowBitrate.store(false, std::memory_order_release);

    // Increment format generation to invalidate cached values in sendAudio
    m_formatGeneration.fetch_add(1, std::memory_order_release);
    // C1: Also increment consumer generation for getNewStream
    m_consumerStateGen.fetch_add(1, std::memory_order_release);

    uint32_t bytesPerSecond = byteRate * channels;
    size_t ringSize = DirettaBuffer::calculateBufferSize(bytesPerSecond, DirettaBuffer::DSD_BUFFER_SECONDS);

    m_ringBuffer.resize(ringSize, 0x69);  // DSD silence
    ringSize = m_ringBuffer.size();

    // Calculate bytesPerBuffer to match DirettaCycleCalculator
    // SDK's m_effectiveMTU already accounts for IP/UDP headers, only Diretta overhead (~3 bytes)
    // Tested by Hoorna: OVERHEAD=3 works at MTU 1500
    constexpr int OVERHEAD = 3;
    int efficientMTU = static_cast<int>(m_effectiveMTU) - OVERHEAD;
    if (efficientMTU < 64) efficientMTU = 1497;  // Fallback (1500 - 3)

    size_t blockSize = 4 * channels;
    size_t bytesPerBuffer = (efficientMTU / blockSize) * blockSize;
    if (bytesPerBuffer < 64) bytesPerBuffer = 64;

    // For low DSD rates where 1ms fits in MTU, use 1ms buffers
    uint32_t inputBytesPerMs = (byteRate / 1000) * channels;
    size_t bytesPerMsAligned = ((inputBytesPerMs + blockSize - 1) / blockSize) * blockSize;
    if (bytesPerMsAligned <= static_cast<size_t>(efficientMTU)) {
        bytesPerBuffer = bytesPerMsAligned;
        DIRETTA_LOG("DSD buffer (1ms): " << bytesPerBuffer << " bytes");
    } else {
        DIRETTA_LOG("DSD buffer (MTU): " << bytesPerBuffer << " bytes");
    }

    m_bytesPerBuffer.store(static_cast<int>(bytesPerBuffer), std::memory_order_release);
    m_bytesPerFrame.store(0, std::memory_order_release);
    m_framesPerBufferRemainder.store(0, std::memory_order_release);
    m_framesPerBufferAccumulator.store(0, std::memory_order_release);

    m_prefillTarget = DirettaBuffer::calculatePrefill(bytesPerSecond, true, false);
    m_prefillTarget = std::min(m_prefillTarget, ringSize / 4);
    m_prefillComplete = false;

    DIRETTA_LOG("Ring DSD: byteRate=" << byteRate << " ch=" << channels
                << " buffer=" << ringSize << " bytesPerBuffer=" << bytesPerBuffer
                << " prefill=" << m_prefillTarget);
}

//=============================================================================
// Playback Control
//=============================================================================

bool DirettaSync::startPlayback() {
    if (!m_open) return false;
    if (m_playing && !m_paused) return true;

    if (m_paused) {
        resumePlayback();
        return true;
    }

    play();
    m_playing = true;
    m_paused = false;
    return true;
}

void DirettaSync::stopPlayback(bool immediate) {
    // Log accumulated underruns at session end
    uint32_t underruns = m_underrunCount.exchange(0, std::memory_order_relaxed);
    if (underruns > 0) {
        std::cerr << "[DirettaSync] Session had " << underruns << " underrun(s)" << std::endl;
    }

    if (!m_playing) return;

    if (!immediate) {
        requestShutdownSilence(m_isDsdMode.load(std::memory_order_acquire) ? 50 : 20);

        auto start = std::chrono::steady_clock::now();
        while (m_silenceBuffersRemaining.load() > 0) {
            if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(150)) break;
            std::this_thread::yield();
        }
    }

    stop();
    m_playing = false;
    m_paused = false;
}

void DirettaSync::pausePlayback() {
    if (!m_playing || m_paused) return;

    requestShutdownSilence(m_isDsdMode.load(std::memory_order_acquire) ? 30 : 10);

    auto start = std::chrono::steady_clock::now();
    while (m_silenceBuffersRemaining.load() > 0) {
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(80)) break;
        std::this_thread::yield();
    }

    stop();
    m_paused = true;
}

void DirettaSync::resumePlayback() {
    if (!m_paused) return;

    DIRETTA_LOG("Resuming from pause...");

    // Reset flags set during pausePlayback()
    m_draining = false;
    m_stopRequested = false;
    m_silenceBuffersRemaining = 0;

    // 不要清除环形缓冲区，保留之前的数据
    // 这样可以立即恢复播放，不需要重新填充

    play();
    m_paused = false;
    m_playing = true;

    DIRETTA_LOG("Resumed - buffer preserved, immediate playback");
}

void DirettaSync::sendPreTransitionSilence() {
    // Pre-transition silence disabled - was causing issues during format switching
    // The stopPlayback() silence mechanism handles this case adequately
}

//=============================================================================
// Audio Data (Push Interface)
//=============================================================================

size_t DirettaSync::sendAudio(const uint8_t* data, size_t numSamples) {
    if (m_draining.load(std::memory_order_acquire)) return 0;
    if (m_stopRequested.load(std::memory_order_acquire)) return 0;

    RingAccessGuard ringGuard(m_ringUsers, m_reconfiguring);
    if (!ringGuard.active()) {
        if (m_isDsdMode.load()) std::cout << "[DirettaSync] DEBUG: sendAudio - Ring guard failed" << std::endl;
        return 0;
    }

    // 增加详细日志，记录音频数据状态
    static int sendAudioCount = 0;
    if (g_verbose && (sendAudioCount++ % 100 == 0)) {
        std::cout << "[DirettaSync] DEBUG: sendAudio - samples=" << numSamples 
                  << " free=" << m_ringBuffer.getFreeSpace()
                  << " available=" << m_ringBuffer.getAvailable()
                  << " dsdMode=" << m_isDsdMode.load()
                  << " pack24bit=" << m_need24BitPack.load()
                  << " shift32bit=" << m_need32BitShift.load() << std::endl;
        
        // 打印前几个音频样本的十六进制值
        if (numSamples > 0 && data) {
            std::cout << "[DirettaSync] DEBUG: First 4 samples: ";
            for (int i = 0; i < std::min(4, static_cast<int>(numSamples)); i++) {
                const uint32_t* sample = reinterpret_cast<const uint32_t*>(data + i * 4);
                std::cout << std::hex << *sample << " " << std::dec;
            }
            std::cout << std::endl;
        }
    }

    // Generation counter optimization: single atomic load vs 5-6 loads
    // Only reload format atomics when format has actually changed
    uint32_t gen = m_formatGeneration.load(std::memory_order_acquire);
    if (gen != m_cachedFormatGen) {
        m_cachedDsdMode = m_isDsdMode.load(std::memory_order_acquire);
        m_cachedPack24bit = m_need24BitPack.load(std::memory_order_acquire);
        m_cachedNeed32BitShift = m_need32BitShift.load(std::memory_order_acquire); // New
        m_cachedUpsample16to32 = m_need16To32Upsample.load(std::memory_order_acquire);
        m_cachedUpsample16to24 = m_need16To24Upsample.load(std::memory_order_acquire);
        m_cachedChannels = m_channels.load(std::memory_order_acquire);
        m_cachedBytesPerSample = m_bytesPerSample.load(std::memory_order_acquire);
        m_cachedDsdConversionMode = m_dsdConversionMode.load(std::memory_order_acquire);
        m_cachedFormatGen = gen;
    }

    // Use cached values (no atomic loads in hot path)
    bool dsdMode = m_cachedDsdMode;
    bool pack24bit = m_cachedPack24bit;
    bool shift32bit = m_cachedNeed32BitShift; // New
    bool upsample16to32 = m_cachedUpsample16to32;
    bool upsample16to24 = m_cachedUpsample16to24;
    int numChannels = m_cachedChannels;
    int bytesPerSample = m_cachedBytesPerSample;

    size_t written = 0;
    size_t totalBytes;
    const char* formatLabel;

    if (dsdMode) {
        // DSD: numSamples encoding from AudioEngine
        // numSamples = (totalBytes * 8) / channels
        // Reverse: totalBytes = numSamples * channels / 8
        totalBytes = (numSamples * numChannels) / 8;
        
        // ⭐ AudioEngine outputs Interleaved 4-byte blocks: [4B L][4B R][4B L][4B R]...
        // Data is in LSB format (DFF already bit-reversed by AudioEngine)
        // We need to apply sink-specific transformations based on m_cachedDsdConversionMode
        
        // Use ring buffer's DSD conversion function which handles Interleaved->Interleaved transformations
        written = m_ringBuffer.pushDSDInterleaved4ByteBlocks(
            data, totalBytes, numChannels, m_cachedDsdConversionMode);
        formatLabel = "DSD";

    } else if (pack24bit) {
        // PCM 24-bit: numSamples is sample count
        size_t bytesPerFrame = 4 * numChannels;  // S24_P32
        totalBytes = numSamples * bytesPerFrame;

        written = m_ringBuffer.push24BitPacked(data, totalBytes);
        formatLabel = "PCM24";

    } else if (shift32bit) {
        // PCM 24-bit in 32-bit container -> 32-bit shift (LSB->MSB)
        size_t bytesPerFrame = 4 * numChannels;  // S24_P32
        totalBytes = numSamples * bytesPerFrame;

        // Process 24-bit audio with 32-bit shift
        written = m_ringBuffer.push32BitShifted(data, totalBytes);
        formatLabel = "PCM32shift";

    } else if (upsample16to32) {
        // PCM 16->32
        size_t bytesPerFrame = 2 * numChannels;
        totalBytes = numSamples * bytesPerFrame;

        written = m_ringBuffer.push16To32(data, totalBytes);
        formatLabel = "PCM16->32";

    } else if (upsample16to24) {
        // PCM 16->24 (sink only supports 24-bit, not 32-bit)
        size_t bytesPerFrame = 2 * numChannels;
        totalBytes = numSamples * bytesPerFrame;

        written = m_ringBuffer.push16To24(data, totalBytes);
        formatLabel = "PCM16->24";

    } else {
        // PCM direct copy
        size_t bytesPerFrame = static_cast<size_t>(bytesPerSample) * numChannels;
        totalBytes = numSamples * bytesPerFrame;

        written = m_ringBuffer.push(data, totalBytes);
        formatLabel = "PCM";
    }

    // Check prefill completion - always check, even if written is 0
    // This fixes a deadlock where buffer is full but prefillComplete never gets set
    if (!m_prefillComplete.load(std::memory_order_acquire)) {
        if (m_ringBuffer.getAvailable() >= m_prefillTarget) {
            m_prefillComplete = true;
            DIRETTA_LOG(formatLabel << " prefill complete: " << m_ringBuffer.getAvailable() << " bytes");
        }
    }

    // 只在处理模式或32位移位状态发生变化时显示状态信息
    static std::string lastFormatLabel;
    static bool lastShift32bit = false;
    
    if (written > 0 && g_verbose) {
        // 当处理模式或32位移位状态发生变化时输出日志
        if (formatLabel != lastFormatLabel || shift32bit != lastShift32bit) {
            DIRETTA_LOG("Audio processing mode: [" << formatLabel << "] shift32bit=" << shift32bit);
            lastFormatLabel = formatLabel;
            lastShift32bit = shift32bit;
        }
    }

    return written;
}

float DirettaSync::getBufferLevel() const {
    RingAccessGuard ringGuard(m_ringUsers, m_reconfiguring);
    if (!ringGuard.active()) return 0.0f;
    size_t size = m_ringBuffer.size();
    if (size == 0) return 0.0f;
    return static_cast<float>(m_ringBuffer.getAvailable()) / static_cast<float>(size);
}

//=============================================================================
// DIRETTA::Sync Overrides
//=============================================================================

bool DirettaSync::getNewStream(diretta_stream& baseStream) {
    // SDK 148 API: Application-managed buffer
    // SDK 148 changed from getNewStream(Stream&) to getNewStream(diretta_stream&)
    // The application must manage memory: allocate buffer, assign to Data.P and Size
    // (Confirmed by Yu Harada: memory management is application's responsibility)
    
    // std::cerr << "[DirettaSync] DEBUG: getNewStream called" << std::endl; // Commented out to avoid spam, uncomment if needed

    m_workerActive = true;

    // C1: Generation counter optimization for stable state
    // Single atomic load in common case (format rarely changes during playback)
    uint32_t gen = m_consumerStateGen.load(std::memory_order_acquire);
    if (gen != m_cachedConsumerGen) {
        // Cold path: reload stable state values
        m_cachedBytesPerBuffer = m_bytesPerBuffer.load(std::memory_order_acquire);
        m_cachedSilenceByte = m_ringBuffer.silenceByte();
        m_cachedConsumerIsDsd = m_isDsdMode.load(std::memory_order_acquire);
        m_cachedConsumerSampleRate = m_sampleRate.load(std::memory_order_acquire);
        // PCM buffer rounding drift fix values (stable per-track)
        m_cachedBytesPerFrame = m_bytesPerFrame.load(std::memory_order_acquire);
        m_cachedFramesPerBufferRemainder = m_framesPerBufferRemainder.load(std::memory_order_acquire);
        m_cachedConsumerGen = gen;
    }

    // Hot path: use cached values
    int currentBytesPerBuffer = m_cachedBytesPerBuffer;
    uint8_t currentSilenceByte = m_cachedSilenceByte;

    // PCM buffer rounding drift fix: accumulator adjusts buffer size for 44.1k family
    // Uses cached remainder/bytesPerFrame, only accumulator is per-call
    if (m_cachedFramesPerBufferRemainder != 0) {
        uint32_t acc = m_framesPerBufferAccumulator.load(std::memory_order_relaxed);
        acc += m_cachedFramesPerBufferRemainder;
        if (acc >= 1000) {
            acc -= 1000;
            currentBytesPerBuffer += m_cachedBytesPerFrame;
        }
        m_framesPerBufferAccumulator.store(acc, std::memory_order_relaxed);
    }

    // SDK 148 WORKAROUND: Use our own buffer instead of Stream::resize()
    // Resize our persistent buffer if needed
    if (m_streamData.size() != static_cast<size_t>(currentBytesPerBuffer)) {
        m_streamData.resize(currentBytesPerBuffer);
    }

    // Directly set the diretta_stream C structure fields
    // The SDK only reads Data.P (pointer) and Size fields
    baseStream.Data.P = m_streamData.data();
    baseStream.Size = currentBytesPerBuffer;

    uint8_t* dest = m_streamData.data();

    RingAccessGuard ringGuard(m_ringUsers, m_reconfiguring);
    if (!ringGuard.active()) {
        std::memset(dest, currentSilenceByte, currentBytesPerBuffer);
        m_workerActive = false;
        return true;
    }

    bool currentIsDsd = m_cachedConsumerIsDsd;
    size_t currentRingSize = m_ringBuffer.size();

    // Shutdown silence
    int silenceRemaining = m_silenceBuffersRemaining.load(std::memory_order_acquire);
    if (silenceRemaining > 0) {
        std::memset(dest, currentSilenceByte, currentBytesPerBuffer);
        m_silenceBuffersRemaining.fetch_sub(1, std::memory_order_acq_rel);
        m_workerActive = false;
        return true;
    }

    // Stop requested
    if (m_stopRequested.load(std::memory_order_acquire)) {
        std::memset(dest, currentSilenceByte, currentBytesPerBuffer);
        m_workerActive = false;
        return true;
    }

    // Prefill not complete
    if (!m_prefillComplete.load(std::memory_order_acquire)) {
        // Check if we have enough data to play, even if prefill is not complete
        size_t avail = m_ringBuffer.getAvailable();
        if (avail < static_cast<size_t>(currentBytesPerBuffer)) {
            // Not enough data yet, return silence
            std::memset(dest, currentSilenceByte, currentBytesPerBuffer);
            m_workerActive = false;
            return true;
        }
        // We have enough data to play, continue with actual audio
    }

    // Post-online stabilization
    // Simplified logic: use a fixed number of buffers for stabilization
    // This ensures quick transition to actual audio data
    if (!m_postOnlineDelayDone.load(std::memory_order_acquire)) {
        int stabilizationTarget = static_cast<int>(DirettaBuffer::POST_ONLINE_SILENCE_BUFFERS);
        
        // Reduce default buffers for faster start - was 20, now 5
        stabilizationTarget = 5;
        
        if (currentIsDsd) {
            // For DSD, use slightly more buffers but still limited
            stabilizationTarget = 10;
        }

        // Clamp to very reasonable range
        stabilizationTarget = std::max(2, std::min(stabilizationTarget, 20));

        int count = m_stabilizationCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count >= stabilizationTarget) {
            m_postOnlineDelayDone = true;
            m_stabilizationCount.store(0, std::memory_order_relaxed);
            DIRETTA_LOG("Post-online stabilization complete (" << count << " buffers)");
        }
        
        // After first few buffers, start passing actual audio data
        // This ensures we don't have long periods of silence
        if (count > stabilizationTarget / 2) {
            // Start passing actual audio data before stabilization is complete
            // This improves perceived start time without affecting stability
        } else {
            std::memset(dest, currentSilenceByte, currentBytesPerBuffer);
            m_workerActive = false;
            return true;
        }
    }

    int count = m_streamCount.fetch_add(1, std::memory_order_relaxed) + 1;
    size_t avail = m_ringBuffer.getAvailable();

    if (g_verbose && (count <= 5 || count % 5000 == 0)) {
        float fillPct = (currentRingSize > 0) ? (100.0f * avail / currentRingSize) : 0.0f;
        // A3: Async logging in hot path - avoids cout blocking in Diretta callback
        DIRETTA_LOG_ASYNC("getNewStream #" << count << " bpb=" << currentBytesPerBuffer
                          << " avail=" << avail << " (" << std::fixed << std::setprecision(1)
                          << fillPct << "%) " << (currentIsDsd ? "[DSD]" : "[PCM]"));
    }

    // Underrun - count silently, log at session end
    if (avail < static_cast<size_t>(currentBytesPerBuffer)) {
        m_underrunCount.fetch_add(1, std::memory_order_relaxed);
        std::memset(dest, currentSilenceByte, currentBytesPerBuffer);
        m_workerActive = false;
        return true;
    }

    // Pop from ring buffer directly into SDK stream
    m_ringBuffer.pop(dest, currentBytesPerBuffer);

    // G1: Signal producer that space is now available
    // Use try_lock to avoid blocking the time-critical consumer thread
    // If producer isn't waiting, this is a no-op (harmless notification)
    if (m_flowMutex.try_lock()) {
        m_flowMutex.unlock();
        m_spaceAvailable.notify_one();
    }

    m_workerActive = false;
    return true;
}

bool DirettaSync::startSyncWorker() {
    std::lock_guard<std::mutex> lock(m_workerMutex);

    DIRETTA_LOG("startSyncWorker (running=" << m_running.load() << ")");

    if (m_running.load() && m_workerThread.joinable()) {
        DIRETTA_LOG("Worker already running");
        return true;
    }

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    m_running = true;
    m_stopRequested = false;

    m_workerThread = std::thread([this]() {
        // F1: Elevate worker thread priority for reduced jitter
        // SCHED_FIFO priority 50 (mid-range real-time) - requires root/CAP_SYS_NICE
        setRealtimePriority(50);

        while (m_running.load(std::memory_order_acquire)) {
            if (!syncWorker()) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
    });

    return true;
}

//=============================================================================
// Internal Helpers
//=============================================================================

void DirettaSync::beginReconfigure() {
    m_reconfiguring.store(true, std::memory_order_release);
    while (m_ringUsers.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }
}

void DirettaSync::endReconfigure() {
    m_reconfiguring.store(false, std::memory_order_release);
}

void DirettaSync::shutdownWorker() {
    m_stopRequested = true;
    m_running = false;

    int waitCount = 0;
    while (m_workerActive.load(std::memory_order_acquire) && waitCount < 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waitCount++;
    }

    std::lock_guard<std::mutex> lock(m_workerMutex);
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void DirettaSync::requestShutdownSilence(int buffers) {
    // N7: Scale silence buffers with DSD rate for consistent flush timing
    // Higher DSD rates have deeper pipelines requiring more buffers
    int scaledBuffers = buffers;
    if (m_isDsdMode.load(std::memory_order_relaxed)) {
        int sampleRate = m_sampleRate.load(std::memory_order_relaxed);
        int dsdMultiplier = sampleRate / 2822400;  // DSD64=1, DSD512=8
        scaledBuffers = buffers * std::max(1, dsdMultiplier);
    }

    m_silenceBuffersRemaining = scaledBuffers;
    m_draining = true;
    DIRETTA_LOG("Requested " << scaledBuffers << " shutdown silence buffers"
                << (scaledBuffers != buffers ? " (scaled from " + std::to_string(buffers) + ")" : ""));
}

bool DirettaSync::waitForOnline(unsigned int timeoutMs) {
    auto start = std::chrono::steady_clock::now();
    auto timeout = std::chrono::milliseconds(timeoutMs);

    while (!is_online()) {
        if (std::chrono::steady_clock::now() - start > timeout) {
            DIRETTA_LOG("Online timeout");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    DIRETTA_LOG("Online after " << elapsed << "ms");
    return true;
}

void DirettaSync::applyTransferMode(DirettaTransferMode mode, ACQUA::Clock cycleTime) {
    if (mode == DirettaTransferMode::AUTO) {
        if (m_isLowBitrate.load(std::memory_order_acquire) ||
            m_isDsdMode.load(std::memory_order_acquire)) {
            DIRETTA_LOG("Using VarAuto");
            configTransferVarAuto(cycleTime);
        } else {
            DIRETTA_LOG("Using VarMax");
            configTransferVarMax(cycleTime);
        }
        return;
    }

    switch (mode) {
        case DirettaTransferMode::FIX_AUTO:
            configTransferFixAuto(cycleTime);
            break;
        case DirettaTransferMode::VAR_AUTO:
            configTransferVarAuto(cycleTime);
            break;
        case DirettaTransferMode::VAR_MAX:
        default:
            configTransferVarMax(cycleTime);
            break;
    }
}

unsigned int DirettaSync::calculateCycleTime(uint32_t sampleRate, int channels, int bitsPerSample) {
    if (!m_config.cycleTimeAuto || !m_calculator) {
        return m_config.cycleTime;
    }
    return m_calculator->calculate(sampleRate, channels, bitsPerSample);
}

//=============================================================================
// Debug Controls
//=============================================================================
void DirettaSync::setDsdByteSwap(bool enable) {
    m_needDsdByteSwap.store(enable, std::memory_order_release);
    
    // Recalculate mode
    bool reverse = m_needDsdBitReversal.load(std::memory_order_acquire);
    DirettaRingBuffer::DSDConversionMode newMode;
    
    if (reverse) {
        newMode = enable ? DirettaRingBuffer::DSDConversionMode::BitReverseAndSwap 
                         : DirettaRingBuffer::DSDConversionMode::BitReverseOnly;
    } else {
        newMode = enable ? DirettaRingBuffer::DSDConversionMode::ByteSwapOnly 
                         : DirettaRingBuffer::DSDConversionMode::Passthrough;
    }
    
    m_dsdConversionMode.store(newMode, std::memory_order_release);
    // ⭐ FORCE UPDATE: Increment generation to invalidate cache in sendAudio
    m_formatGeneration.fetch_add(1, std::memory_order_release);
    
    std::cout << "[DirettaSync] DEBUG: DSD ByteSwap=" << enable << " Mode=" << static_cast<int>(newMode) << std::endl;
    DIRETTA_LOG("DEBUG: DSD Mode updated to " << static_cast<int>(newMode));
}

void DirettaSync::setDsdBitReverse(bool enable) {
    m_needDsdBitReversal.store(enable, std::memory_order_release);
    
    // Recalculate mode
    bool swap = m_needDsdByteSwap.load(std::memory_order_acquire);
    DirettaRingBuffer::DSDConversionMode newMode;
    
    if (enable) {
        newMode = swap ? DirettaRingBuffer::DSDConversionMode::BitReverseAndSwap 
                       : DirettaRingBuffer::DSDConversionMode::BitReverseOnly;
    } else {
        newMode = swap ? DirettaRingBuffer::DSDConversionMode::ByteSwapOnly 
                       : DirettaRingBuffer::DSDConversionMode::Passthrough;
    }
    
    m_dsdConversionMode.store(newMode, std::memory_order_release);
    // ⭐ FORCE UPDATE: Increment generation to invalidate cache in sendAudio
    m_formatGeneration.fetch_add(1, std::memory_order_release);
    
    std::cout << "[DirettaSync] DEBUG: DSD BitReverse=" << enable << " Mode=" << static_cast<int>(newMode) << std::endl;
    DIRETTA_LOG("DEBUG: DSD Mode updated to " << static_cast<int>(newMode));
}
