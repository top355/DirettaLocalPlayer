#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <unistd.h> // For getpid()
#include <vector>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <cstdlib> // For rand()
#include <random>
#include <cmath>
#include <numeric>

#include <sys/file.h>
#include <fcntl.h>
#include <errno.h>

// #include "DirettaOutput.h" // REMOVED

#include "AudioEngine.h"
#include "IPC.h"
#include "CueParser.h"

// ============================================================================
// Global State
// ============================================================================
#include "PlayerManager.h"

// For debug logging - used by AudioEngine and DirettaOutput
// For debug logging - used by AudioEngine and DirettaOutput
bool g_verbose = true; // Enable for debugging
// Global log ring for async logging (used by DirettaSync)
// Defined in DirettaSync.h as extern
#include "DirettaSync.h"
LogRing* g_logRing = nullptr;


void signalHandler(int signum) {
    static int signalCount = 0;
    signalCount++;
    
    PlayerManager& manager = PlayerManager::getInstance();
    
    if (signalCount == 1) {
        if (manager.isDaemonMode()) {
            std::cout << "\n[Daemon] Caught signal " << signum << ", stopping gracefully..." << std::endl;
            std::cout << "[Daemon] Press Ctrl+C again to force exit" << std::endl;
        }
        manager.setRunning(false);
    } else {
        // Second signal - force exit
        std::cout << "\n[Daemon] Force exit!" << std::endl;
        _exit(0);  // Immediate exit, no cleanup
    }
}

// ============================================================================
// Helper Functions
// ============================================================================
std::string formatDuration(double seconds) {
    int s = static_cast<int>(seconds);
    int m = s / 60;
    s = s % 60;
    std::stringstream ss;
    ss << std::setfill('0') << std::setw(2) << m << ":" 
       << std::setfill('0') << std::setw(2) << s;
    return ss.str();
}

// ============================================================================
// Client Mode
// ============================================================================
int ClientMain(const std::vector<std::string>& args) {
    std::string response = IPCClient::sendCommand(args);
    std::cout << response << std::endl; // Standard Output for JS to capture
    return 0;
}

// ============================================================================
// Daemon Mode - Command Handlers
// ============================================================================
#include <filesystem>
namespace fs = std::filesystem;

// ...

// ============================================================================
// Playlist Logic
// ============================================================================
int GetNextIndex(bool autoAdvance) {
    PlayerManager& manager = PlayerManager::getInstance();
    auto& playlist = manager.getPlaylist();
    auto& shuffleList = manager.getShuffleList();
    int playlistIndex = manager.getPlaylistIndex();
    
    if (playlist.empty()) return -1;
    
    // Get modes from engine
    auto& engine = manager.getEngine();
    auto repeat = engine.getRepeatMode();
    bool random = engine.getRandomMode();
    
    // Random Mode
    if (random) {
        // [Feature] True Shuffle Logic
        // 1. Generate if empty or size mismatch
        if (shuffleList.size() != playlist.size()) {
            shuffleList.resize(playlist.size());
            std::iota(shuffleList.begin(), shuffleList.end(), 0);
            std::random_device rd;
            std::mt19937 g(rd());
            std::shuffle(shuffleList.begin(), shuffleList.end(), g);
        }
        
        // 2. Find current track in shuffle list
        auto it = std::find(shuffleList.begin(), shuffleList.end(), playlistIndex);
        int pos = -1;
        if (it != shuffleList.end()) {
            pos = std::distance(shuffleList.begin(), it);
        }
        
        // 3. Pick next
        int nextPos = pos + 1;
        if (nextPos >= (int)shuffleList.size()) {
            // End of shuffle list
            if (repeat == AudioEngine::RepeatMode::ALL) {
                 // Reshuffle for next round
                 std::random_device rd;
                 std::mt19937 g(rd());
                 std::shuffle(shuffleList.begin(), shuffleList.end(), g);
                 
                 // Cosmetic: Try to avoid immediate repeat if possible
                 if (!shuffleList.empty() && shuffleList[0] == playlistIndex && shuffleList.size() > 1) {
                     std::swap(shuffleList[0], shuffleList.back());
                 }
                 return shuffleList[0];
            }
            return -1; // Stop
        }
        return shuffleList[nextPos];
    }
    
    // Repeat One (only valid for autoAdvance)
    if (autoAdvance && repeat == AudioEngine::RepeatMode::ONE) {
        return playlistIndex;
    }
    
    // Normal Next
    int next = playlistIndex + 1;
    
    // End of Playlist
    if (next >= (int)playlist.size()) {
        if (repeat == AudioEngine::RepeatMode::ALL) {
            return 0; // Loop back
        }
        return -1; // Stop
    }
    
    return next;
}

int GetPrevIndex() {
    PlayerManager& manager = PlayerManager::getInstance();
    auto& playlist = manager.getPlaylist();
    int playlistIndex = manager.getPlaylistIndex();
    
    if (playlist.empty()) return -1;
    
    // If random, prev is tricky without history. 
    // For now, just go to previous index or loop.
    int prev = playlistIndex - 1;
    if (prev < 0) {
        return playlist.size() - 1; // Wrap around? Or stop? usually wrap for prev
    }
    return prev;
}

// ============================================================================
// Helper: Scan Tracks
// ============================================================================
// ============================================================================
// Helper: Scan Tracks
// ============================================================================

// ⭐ Helper to get duration using FFmpeg (lightweight open)
static double GetAudioFileDuration(const std::string& path) {
    if (path.empty()) return 0.0;
    
    // Suppress FFmpeg logs
    // av_log_set_level(AV_LOG_QUIET);
    
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr) < 0) {
        return 0.0;
    }
    
    // Some formats (like DSD/DSF) might need stream info to get duration
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx);
        return 0.0;
    }
    
    double duration = 0.0;
    if (fmt_ctx->duration != AV_NOPTS_VALUE) {
        duration = static_cast<double>(fmt_ctx->duration) / AV_TIME_BASE;
    }
    
    avformat_close_input(&fmt_ctx);
    return duration;
}

// ⭐ 辅助函数：不区分大小写的路径搜索
static std::string FindFileIgnoreCase(const std::string& directory, const std::string& filename) {
    try {
        if (filename.empty()) return "";
        std::string target = filename;
        std::transform(target.begin(), target.end(), target.begin(), ::tolower);

        if (!fs::exists(directory)) return filename;

        for (const auto& entry : fs::directory_iterator(directory)) {
            std::string current = entry.path().filename().string();
            std::string lowerCurrent = current;
            std::transform(lowerCurrent.begin(), lowerCurrent.end(), lowerCurrent.begin(), ::tolower);
            if (lowerCurrent == target) {
                return current; // 返回磁盘上真实的文件名
            }
        }
    } catch (...) {}
    return filename; // 找不到则返回原始名，交由后续逻辑报错
}

std::vector<TrackInfo> ScanTracks(const std::string& path) {
    std::vector<TrackInfo> tracks;
    try {
        // ⭐ DEBUG: Log path received
//        std::cout << "[ScanTracks] Processing path: '" << path << "'" << std::endl;
        
        if (!fs::exists(path)) {
            std::cerr << "[ScanTracks] Path NOT found: '" << path << "'" << std::endl;
        }
        
        // Strict extension check to prevent binary files being parsed as CUE
        std::string ext = fs::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        
        if (ext == ".cue") {
            CueSheet sheet = CueParser::parse(path);
            
            // ⭐ 增强：计算基础目录，剥离 .utf8.cue 等可能由 Web 端产生的后缀
            std::string basePath = path;
            size_t lastSlash = basePath.find_last_of('/');
            if (lastSlash != std::string::npos) {
                basePath = basePath.substr(0, lastSlash + 1);
            } else {
                basePath = "./";
            }

            // 获取音频文件总时长
            double totalDuration = 0.0;
            if (!sheet.tracks.empty()) {
                std::string realFilename = FindFileIgnoreCase(basePath, sheet.tracks[0].file);
                std::string audioPath = basePath + realFilename;
                totalDuration = GetAudioFileDuration(audioPath);
            }
            
            for (const auto& t : sheet.tracks) {
                TrackInfo info;
                // 拼接绝对路径
                std::string realFilename = FindFileIgnoreCase(basePath, t.file);
                info.path = basePath + realFilename;
                
                // 验证文件是否存在
                if (!fs::exists(info.path)) {
                    std::cerr << "[ScanTracks] ⚠️ Warning: Referenced file not found: " << info.path << " (Original: " << t.file << ")" << std::endl;
                }
                
                info.startTime = t.startTime;
                info.virtualDuration = t.duration;
                info.trackTitle = t.title;
                info.trackArtist = t.performer;
                tracks.push_back(info);
            }
            
            // 修复最后一首曲目时长为0的问题
            if (!tracks.empty()) {
                TrackInfo& lastTrack = tracks.back();
                if (lastTrack.virtualDuration <= 0.0 && totalDuration > lastTrack.startTime) {
                    lastTrack.virtualDuration = totalDuration - lastTrack.startTime;
                }
            }
        } else {
            std::vector<std::string> files;
            // Check if directory
            if (fs::is_directory(path)) {
                for (const auto& entry : fs::directory_iterator(path)) {
                    if (!entry.is_regular_file()) continue;
                    
                    std::string e = entry.path().extension().string();
                    std::string extLow = e; 
                    std::transform(extLow.begin(), extLow.end(), extLow.begin(), ::tolower);
                    
                    // Simple filter
                    if (extLow == ".flac" || extLow == ".wav" || extLow == ".dsf" || extLow == ".dff" || extLow == ".mp3") {
                        files.push_back(entry.path().string());
                    }
                }
                std::sort(files.begin(), files.end());
            } else if (fs::exists(path)) {
                // Single file
                files.push_back(path);
            }
            
            // ⭐ Process files with duration accumulation
            double runningTime = 0.0;
            
            for (const auto& f : files) {
                TrackInfo info;
                info.path = f;
                info.trackTitle = fs::path(f).filename().string();
                
                // ⭐ Get actual duration
                double d = GetAudioFileDuration(f);
                if (d > 0.0) {
                    info.virtualDuration = d;
                    info.durationSeconds = d; // Set both for compatibility
                }
                
                // ⭐ For individual audio files, startTime should always be 0
                // Only CUE virtual tracks have non-zero startTime values
                info.startTime = 0; // Always start from beginning for individual files
                runningTime += d;
                
                tracks.push_back(info);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[ScanTracks] Error: " << e.what() << std::endl;
    }
    return tracks;
}

// ============================================================================
// Helper: Join Args (for paths with spaces)
// ============================================================================
std::string JoinArgs(const std::vector<std::string>& args, size_t startIdx) {
    std::string res;
    for (size_t i = startIdx; i < args.size(); ++i) {
        if (i > startIdx) res += " ";
        res += args[i];
    }
    return res;
}

// ============================================================================
// Helper: Update Next Track (Gapless)
// ============================================================================
void UpdateNextTrack() {
    std::thread([](){
         PlayerManager& manager = PlayerManager::getInstance();
         std::lock_guard<std::mutex> lock(manager.getPlaylistMutex());
         int nextIdx = GetNextIndex(true);
         if (nextIdx >= 0 && nextIdx < (int)manager.getPlaylist().size()) {
             std::cout << "[Daemon] Mode Changed: Queueing next track " << nextIdx << ": " << manager.getPlaylist()[nextIdx].path << std::endl;
             manager.getEngine().setNextTrack(manager.getPlaylist()[nextIdx]);
         } else {
             manager.getEngine().setNextURI("");
         }
    }).detach();
}

std::string HandleCommand(const std::vector<std::string>& args) {
    if (args.empty()) return "Error: No command";
    
    std::string cmd = args[0];
    
    // --- LIST (Hosts) ---
    if (cmd == "list") {
        return "Localhost Local Player (Daemon)";
    }
    
    // --- TARGET (Targets) ---    
    if (cmd == "target") {
        // 使用 Diretta::Find API 查找目标设备
        DIRETTA::Find::Setting findSetting;
        findSetting.Loopback = false;
        findSetting.ProductID = 0;
        
        DIRETTA::Find find(findSetting);
        
        if (!find.open()) {
            return "Error: Failed to open Find";
        }
        
        DIRETTA::Find::PortResalts targets;
        if (!find.findOutput(targets)) {
            return "Error: Failed to find outputs";
        }
        
        if (targets.empty()) {
            return "No targets found";
        }
        
        // 构建 target 列表字符串，格式: Target1 Diretta Target (设备名称)
        std::string result;
        int index = 1;
        for (const auto& target : targets) {
            result += "Target" + std::to_string(index) + " Diretta Target (" + target.second.targetName + ")\n";
            index++;
        }
        
        // 移除最后一个换行符
        if (!result.empty()) {
            result.pop_back();
        }
        
        return result;
    }
    
    // --- CONNECT ---    
    if (cmd == "connect") {
        if (args.size() > 1) {
            // 获取target索引
            std::string targetIndexStr = args[1];
            try {
                int targetIndex = std::stoi(targetIndexStr);
                
                // [FIX] Index Base Correction
                // User sees 1-based Index (1, 2, 3...)
                // 0 is reserved for "Auto Connect"
                // Internal Vector uses 0-based Index (0, 1, 2...)
                int internalIndex = -1;
                if (targetIndex > 0) {
                    internalIndex = targetIndex - 1;
                }

                PlayerManager& manager = PlayerManager::getInstance();
                auto& engine = manager.getEngine();
                
                // 设置目标索引，AudioEngine会在播放时使用这个索引连接设备
                engine.setDirettaTargetIndex(internalIndex);
                
                return "Connected to Target " + targetIndexStr + " (PID: " + std::to_string(getpid()) + ")";
            } catch (...) {
                return "Error: Invalid target index";
            }
        }
        return "Error: Missing target (Usage: connect <index>)";
    }

    // --- CURRENT TARGET ---
    if (cmd == "current_target") {
        std::stringstream ss;
        ss << "Target: Auto (DirettaSync)";
        ss << " [PID: " << getpid() << "]";
        return ss.str();
    }

    // --- UPLOAD (Load Playlist/Folder - Replace Mode) ---
    if (cmd == "upload") {
        if (args.size() < 2) return "Error: Missing path";
        
        // 解析路径：如果第二个参数是host（不包含路径分隔符），则从第三个参数开始获取路径
        int pathStartIdx = 1;
        if (args.size() > 2 && args[1].find('/') == std::string::npos && args[1].find('\\') == std::string::npos) {
            // 第二个参数是host，路径从第三个参数开始
            pathStartIdx = 2;
        }
        
        std::string path = JoinArgs(args, pathStartIdx);
        
        // ⭐ DEBUG: Show exact joined path
        std::cout << "[Command:upload] Joined path: '" << path << "'" << std::endl;

        std::vector<TrackInfo> newTracks = ScanTracks(path);
        if (newTracks.empty()) return "Error: No tracks found or scan failed for path: " + path;

        PlayerManager& manager = PlayerManager::getInstance();
        std::lock_guard<std::mutex> lock(manager.getPlaylistMutex());
        manager.getPlaylist() = std::move(newTracks);
        manager.setPlaylistIndex(0);
        manager.getShuffleList().clear(); // Reset shuffle
        return "Loaded " + std::to_string(manager.getPlaylist().size()) + " tracks";
    }

    // --- ADD (Append to Playlist) ---
    if (cmd == "add") {
        if (args.size() < 2) return "Error: Missing path";
        std::string path = JoinArgs(args, 1);
        
        std::vector<TrackInfo> newTracks = ScanTracks(path);
        if (newTracks.empty()) return "Error: No tracks found";

        PlayerManager& manager = PlayerManager::getInstance();
        std::lock_guard<std::mutex> lock(manager.getPlaylistMutex());
        // size_t oldSize = manager.getPlaylist().size();
        
        // ⭐ For individual audio files, don't accumulate startTime
        // Only CUE virtual tracks should have non-zero startTime values
        // So we don't modify startTime here, it's already set correctly in ScanTracks
        // double lastEndTime = 0.0;
        // if (!manager.getPlaylist().empty()) {
        //     const auto& last = manager.getPlaylist().back();
        //     lastEndTime = last.startTime + (last.virtualDuration > 0 ? last.virtualDuration : 0.0);
        // }
        
        // for (auto& t : newTracks) {
        //     t.startTime += lastEndTime;
        //     if (t.virtualDuration > 0) {
        //         lastEndTime += t.virtualDuration;
        //     }
        // }
        
        manager.getPlaylist().insert(manager.getPlaylist().end(), newTracks.begin(), newTracks.end());
        manager.getShuffleList().clear(); // Reset shuffle on add
        
        return "Added " + std::to_string(newTracks.size()) + " tracks. Total: " + std::to_string(manager.getPlaylist().size());
    }

    // --- REMOVE (Remove Track) ---
    if (cmd == "remove") {
         if (args.size() < 2) return "Error: Missing index";
         try {
             int idx = std::stoi(args[1]);
             PlayerManager& manager = PlayerManager::getInstance();
             std::lock_guard<std::mutex> lock(manager.getPlaylistMutex());
             auto& playlist = manager.getPlaylist();
             auto& shuffleList = manager.getShuffleList();
             auto& engine = manager.getEngine();
             
             if (idx >= 0 && idx < (int)playlist.size()) {
                 playlist.erase(playlist.begin() + idx);
                 
                 // Adjust current index
                 int playlistIndex = manager.getPlaylistIndex();
                 if (idx < playlistIndex) {
                     manager.setPlaylistIndex(playlistIndex - 1);
                 } else if (idx == playlistIndex) {
                     // Removed currently playing track?
                     // Keep index, but it now points to the next track.
                     // If we were at end, wrap or clamp?
                     if (playlistIndex >= (int)playlist.size()) {
                         manager.setPlaylistIndex(0); // Reset to start if empty or last removed
                         if (playlist.empty()) engine.stop(); 
                     }
                 }
                 shuffleList.clear(); // Clear shuffle list to regenerate on next play
                 UpdateNextTrack(); // Fix: Update next track after removal
                 return "Removed track " + std::to_string(idx);
             }
         } catch (...) { return "Error: Invalid index"; }
         return "Error: Index out of range";
    }
    
    // --- CLEAR (Empty Playlist) ---
    if (cmd == "clear") {
        PlayerManager& manager = PlayerManager::getInstance();
        std::lock_guard<std::mutex> lock(manager.getPlaylistMutex());
        manager.getPlaylist().clear();
        manager.getShuffleList().clear();
        manager.setPlaylistIndex(0);
        manager.getEngine().stop();
        return "Playlist cleared";
    }
    
    // --- INFO (Track Info) ---
    if (cmd == "info") {
        PlayerManager& manager = PlayerManager::getInstance();
        std::lock_guard<std::mutex> lock(manager.getPlaylistMutex());
        auto& playlist = manager.getPlaylist();
        int playlistIndex = manager.getPlaylistIndex();
        
        if (playlistIndex >= 0 && playlistIndex < (int)playlist.size()) {
            const auto& t = playlist[playlistIndex];
            std::stringstream ss;
            ss << "Title=" << t.trackTitle << "\n";
            ss << "Artist=" << t.trackArtist << "\n";
            ss << "Path=" << t.path << "\n";
            ss << "SampleRate=" << t.sampleRate << "\n";
            ss << "BitDepth=" << t.bitDepth << "\n";
            return ss.str();
        }
        return "Error: No track loaded";
    }

    // --- VOLUME ---
    if (cmd == "volume") {
        PlayerManager& manager = PlayerManager::getInstance();
        if (args.size() > 1) {
            try {
                int vol = std::stoi(args[1]);
                manager.getEngine().setVolume(vol);
                return "Volume " + std::to_string(vol);
            } catch (...) { return "Error: Invalid volume"; }
        }
        return "Volume " + std::to_string(manager.getEngine().getVolume());
    }


    // --- REPEAT ---
    if (cmd == "repeat") {
        if (args.size() > 1) {
            std::string mode = args[1];
            PlayerManager& manager = PlayerManager::getInstance();
            auto& engine = manager.getEngine();
            if (mode == "one") engine.setRepeatMode(AudioEngine::RepeatMode::ONE);
            else if (mode == "all") engine.setRepeatMode(AudioEngine::RepeatMode::ALL);
            else engine.setRepeatMode(AudioEngine::RepeatMode::OFF);
            UpdateNextTrack(); // [FIX] Immediately act on mode change
            return "Repeat " + mode;
        }
        return "Repeat Status Query"; // Or return current?
    }

    // --- RANDOM ---
    if (cmd == "random") {
         if (args.size() > 1) {
            bool on = (args[1] == "1" || args[1] == "on" || args[1] == "true");
            PlayerManager& manager = PlayerManager::getInstance();
            auto& engine = manager.getEngine();
            engine.setRandomMode(on);
            if (on) {
                 // Force regenerate shuffle list when enabling
                 std::lock_guard<std::mutex> lock(manager.getPlaylistMutex());
                 manager.getShuffleList().clear();
            }
            UpdateNextTrack(); // [FIX] Immediately act on mode change
            return "Random " + std::string(on ? "On" : "Off");
        }
        return "Random Status Query";
    }

    // --- TAG (List Tracks) ---
    if (cmd == "tag") {
        PlayerManager& manager = PlayerManager::getInstance();
        std::lock_guard<std::mutex> lock(manager.getPlaylistMutex());
        std::stringstream ss;
        for (size_t i = 0; i < manager.getPlaylist().size(); ++i) {
            const auto& t = manager.getPlaylist()[i];
            int64_t startSec = static_cast<int64_t>(t.startTime);
            int64_t durSec = static_cast<int64_t>(t.virtualDuration > 0 ? t.virtualDuration : 0);
            
            // ⭐ Extended Tag Format: Index:StartTime:Duration:Title
            // If client only reads 3 parts, this appends Duration before Title, which might break Title.
            // Better to append at end? Index:StartTime:Title:Duration?
            // Safer: Index:StartTime:Title (Standard)
            // But client uses StartTime delta to calculate duration?
            // If we provide correct accumulated StartTime (via ScanTracks change),
            // then client should be able to calculate duration correctly.
            //
            // However, to be extra helpful, let's try appending Duration if client supports it?
            // Let's stick to Standard Format first, but with CORRECT StartTime relative to previous tracks.
            //
            // Wait, previously `tag` just output `Index:StartTime:Title`.
            // Now that we have correct cumulative `startTime`, the client might work automatically.
            // BUT, let's also add duration as a 4th field just in case.
             ss << "Tag=" << i << ":" << startSec << ":" << t.trackTitle << ":" << durSec << "\n";
        }
        return ss.str();
    }
    
    // --- SEEKTAG (Play specific track) ---
    if (cmd == "seektag") {
        if (args.size() < 2) return "Error: Missing index";
        std::string idxStr = args.back();
        try {
            int idx = std::stoi(idxStr);
            PlayerManager& manager = PlayerManager::getInstance();
            std::lock_guard<std::mutex> lock(manager.getPlaylistMutex());
            if (idx >= 0 && idx < (int)manager.getPlaylist().size()) {
                manager.setPlaylistIndex(idx);
                manager.getEngine().playTrack(manager.getPlaylist()[idx]);
                UpdateNextTrack(); // Fix: Update next track when switching to specific track
                return "Playing track " + std::to_string(idx);
            }
        } catch(...) { return "Error: Invalid index"; }
        return "Error: Index out of range";
    }
    
    // --- PLAY (Resume or Start) ---
    if (cmd == "play") {
        PlayerManager& manager = PlayerManager::getInstance();
        auto& engine = manager.getEngine();
        // If we are stopped, play current index
        if (engine.getState() == AudioEngine::State::STOPPED) {
             std::lock_guard<std::mutex> lock(manager.getPlaylistMutex());
             if (manager.getPlaylistIndex() >= 0 && manager.getPlaylistIndex() < (int)manager.getPlaylist().size()) {
                 engine.playTrack(manager.getPlaylist()[manager.getPlaylistIndex()]);
                 UpdateNextTrack(); // Fix: Update next track when starting playback
                 return "Playing";
             }
             return "Error: Playlist empty or validation failed";
        }
        // If paused, resume
        if (engine.play()) return "Resumed";
        return "Already Playing";
    }

    // --- PAUSE ---
    if (cmd == "pause") {
        PlayerManager::getInstance().getEngine().pause();
        return "Paused";
    }

    // --- STOP ---
    if (cmd == "stop") {
        std::cout << "[Command] STOP command received" << std::endl;
        PlayerManager& manager = PlayerManager::getInstance();
        manager.getEngine().stop();
        
        return "Stopped";
    }

    // --- NEXT ---
    if (cmd == "next") {
        PlayerManager& manager = PlayerManager::getInstance();
        std::lock_guard<std::mutex> lock(manager.getPlaylistMutex());
        int next = GetNextIndex(false); // force next logic (ignore Repeat One)
        if (next >= 0) {
             manager.setPlaylistIndex(next);
             manager.getEngine().playTrack(manager.getPlaylist()[next]);
             UpdateNextTrack(); // Fix: Update next track when switching to next track
             return "Playing Next " + std::to_string(manager.getPlaylistIndex());
        }
        return "End of Playlist";
    }

    // --- PREV ---
    if (cmd == "prev") {
        PlayerManager& manager = PlayerManager::getInstance();
        std::lock_guard<std::mutex> lock(manager.getPlaylistMutex());
        int prev = GetPrevIndex();
        if (prev >= 0) {
             manager.setPlaylistIndex(prev);
             manager.getEngine().playTrack(manager.getPlaylist()[prev]);
             UpdateNextTrack(); // Fix: Update next track when switching to previous track
             return "Playing Prev " + std::to_string(manager.getPlaylistIndex());
        }
        return "At Start";
    }
    
    // --- SEEK ---
    if (cmd == "seek") {
         if (args.size() < 2) return "Error";
         std::string val = args.back();
         PlayerManager::getInstance().getEngine().seek(val); 
         return "Seek " + val;
    }

    // --- AB LOOP ---
    if (cmd == "ab") {
        if (args.size() < 2) return "Error: Missing ab sub-command (a, b, toggle, clear, status)";
        std::string sub = args[1];
        PlayerManager& manager = PlayerManager::getInstance();
        auto& engine = manager.getEngine();
        
        if (sub == "a") {
            double pos = engine.getPosition();
            engine.setABA(pos);
            return "AB-A set to " + std::to_string(pos);
        } else if (sub == "b") {
            double pos = engine.getPosition();
            engine.setABB(pos);
            return "AB-B set to " + std::to_string(pos);
        } else if (sub == "toggle") {
            bool current = engine.isABLoopActive();
            engine.setABLoop(!current);
            return std::string("AB-Loop ") + (!current ? "On" : "Off");
        } else if (sub == "clear") {
            engine.clearAB();
            return "AB-Loop cleared";
        } else if (sub == "status") {
            std::stringstream ss;
            ss << "Active=" << (engine.isABLoopActive() ? "1" : "0") << "\n";
            ss << "A=" << engine.getABA() << "\n";
            ss << "B=" << engine.getABB() << "\n";
            return ss.str();
        }
        return "Error: Unknown ab sub-command";
    }
    
    // --- TARGET ---
    if (cmd == "target") {
        if (args.size() < 2) return "Error: Missing sub-command";
        std::string sub = args[1];
        
        if (sub == "list") {
             // Return JSON list of targets
             return DirettaSync::getTargetsJson();
        } else if (sub == "set" && args.size() >= 3) {
             std::string ip = args[2];
             PlayerManager::getInstance().getEngine().setDirettaTargetIP(ip);
             return "Target set to " + ip;
        }
        return "Error: Unknown target sub-command";
    }

    // --- DSD DEBUG ---
    if (cmd == "dsd_swap") {
        if (args.size() < 2) return "Usage: dsd_swap [0|1]";
        bool on = (args[1] == "1" || args[1] == "true");
        PlayerManager::getInstance().getEngine().setDsdByteSwap(on);
        return std::string("DSD ByteSwap ") + (on ? "ON" : "OFF");
    }
    if (cmd == "dsd_reverse") {
        if (args.size() < 2) return "Usage: dsd_reverse [0|1]";
        bool on = (args[1] == "1" || args[1] == "true");
        PlayerManager::getInstance().getEngine().setDsdBitReverse(on);
        return std::string("DSD BitReverse ") + (on ? "ON" : "OFF");
    }

    // --- STATUS ---
    if (cmd == "status") {
        std::stringstream ss;
        PlayerManager& manager = PlayerManager::getInstance();
        auto& engine = manager.getEngine();
        auto state = engine.getState();
        std::string stateStr = (state == AudioEngine::State::PLAYING) ? "Play" :
                               (state == AudioEngine::State::PAUSED)  ? "Pause" : "Stop";
        
        ss << "State=" << stateStr << "\n";
        ss << "LastTime=" << static_cast<int64_t>(engine.getPosition()) << "\n";
        // Extended Info
        ss << "Repeat=" << (int)engine.getRepeatMode() << "\n"; // 0,1,2
        ss << "Random=" << (engine.getRandomMode() ? "1" : "0") << "\n";
        
        // AB Status in regular status
        ss << "ABActive=" << (engine.isABLoopActive() ? "1" : "0") << "\n";
        ss << "ABA=" << engine.getABA() << "\n";
        ss << "ABB=" << engine.getABB() << "\n";

        std::lock_guard<std::mutex> lock(manager.getPlaylistMutex());
        ss << "Tag=" << manager.getPlaylistIndex() << "\n";

        if (manager.getPlaylistIndex() >= 0 && manager.getPlaylistIndex() < (int)manager.getPlaylist().size()) {
            const auto& playlistEntry = manager.getPlaylist()[manager.getPlaylistIndex()];
            
            // ⭐ 终极修复：优先使用引擎实时获取的轨道信息，而不是扫描时的预测信息
            TrackInfo realInfo = engine.getCurrentTrackInfo();
            
            // 如果文件名匹配（确认为正在播的分轨/文件），则合并时长数据
            // 注意：对于 CUE 来说，path 是一样的，所以总是能匹配上。
            ss << "AudioFmt=" << realInfo.bitDepth << "bit/" << realInfo.sampleRate << "Hz\n";
            ss << "ContextName=" << playlistEntry.trackArtist << "\n"; 
            ss << "Title=" << playlistEntry.trackTitle << "\n";
            ss << "StartTime=" << static_cast<int64_t>(playlistEntry.startTime) << "\n";
            
            // 决策逻辑：如果是 CUE 分轨且 virtualDuration > 0，取分轨时长；
            // 否则（如整轨最后一首或普通文件），取引擎探测出的 durationSeconds
            double dur = (playlistEntry.virtualDuration > 0) ? playlistEntry.virtualDuration : realInfo.durationSeconds;
            ss << "Duration=" << static_cast<int64_t>(dur) << "\n";
        }
        
        // ⭐ 音频状态信息：用于后台数据分析音频是否正常发出
        DirettaSync* sync = engine.getDirettaSync();
        if (sync) {
            // 获取环形缓冲区状态
            size_t bufferAvailable = sync->getRingBufferAvailable();
            size_t bufferSize = sync->getRingBufferSize();
            float bufferLevel = bufferSize > 0 ? static_cast<float>(bufferAvailable) / bufferSize * 100.0f : 0.0f;
            
            ss << "BufferLevel=" << std::fixed << std::setprecision(1) << bufferLevel << "%\n";
            ss << "BufferAvailable=" << bufferAvailable << "\n";
            ss << "BufferSize=" << bufferSize << "\n";
            ss << "PrefillComplete=" << (sync->isPrefillComplete() ? "1" : "0") << "\n";
            ss << "UnderrunCount=" << sync->getUnderrunCount() << "\n";
            // ⭐ Fix: Use more accurate logic to determine actual playing status
            // Prioritize DirettaSync's actual playing state, then use fallback logic
            // only when buffer is healthy and no underruns
            bool isSyncPlaying = sync->isPlaying();
            bool isEnginePlaying = (state == AudioEngine::State::PLAYING);
            bool isBufferHealthy = (bufferLevel > 50.0f);
            bool isPrefillDone = sync->isPrefillComplete();
            bool noUnderruns = (sync->getUnderrunCount() == 0);
            
            bool isActuallyPlaying = isSyncPlaying || 
                                    (isEnginePlaying && isBufferHealthy && isPrefillDone && noUnderruns);
            ss << "Playing=" << (isActuallyPlaying ? "1" : "0") << "\n";
        }
        
        return ss.str();
    }
    
    return "Unknown Command: " + cmd;
}

// ============================================================================
// Daemon Main
// ============================================================================
int DaemonMain() {
    // ⭐ beta10 FIX: Single Instance Lock
    const char* PID_FILE = "/tmp/diretta_player.pid";
    int pidFile = open(PID_FILE, O_CREAT | O_RDWR, 0666);
    if (pidFile != -1) {
        if (flock(pidFile, LOCK_EX | LOCK_NB) == -1) {
            // Already locked
            std::cerr << "[Daemon] ❌ Another instance is already running!" << std::endl;
            
            // Try to read PID
            char pidBuf[16] = {0};
            if (pread(pidFile, pidBuf, sizeof(pidBuf)-1, 0) > 0) {
                int oldPid = atoi(pidBuf);
                std::cerr << "[Daemon]    Old PID: " << oldPid << std::endl;
                std::cerr << "[Daemon]    Please run: pkill -9 DirettaLocalPlayer" << std::endl;
                 // Option: Auto-kill? For now, just exit to stay safe.
            }
            return 1;
        } else {
            // Got lock, write our PID
            ftruncate(pidFile, 0);
            std::string pidStr = std::to_string(getpid()) + "\n";
            write(pidFile, pidStr.c_str(), pidStr.length());
            // Don't close pidFile here, keep it open to hold the lock
        }
    }

    PlayerManager& manager = PlayerManager::getInstance();
    manager.setDaemonMode(true);
    std::cout << "[Daemon] Starting DirettaLocalPlayer Service..." << std::endl;
    
    // 1. Start IPC Server
    IPCServer ipc;
    if (!ipc.start(HandleCommand)) {
        std::cerr << "[Daemon] Failed to start IPC Server on " << IPC_SOCKET_PATH << std::endl;
        return 1;
    }
    std::cout << "[Daemon] IPC Server listening on " << IPC_SOCKET_PATH << std::endl;
    
    // 2. Init Audio Engine Callbacks
    // AudioCallback is no longer used for output (AudioEngine handles DirettaSync internally)
    // We can leave it unset or set an empty one.
    // manager.getEngine().setAudioCallback(...);
    
    manager.getEngine().setTrackChangeCallback([](int /*trackNum*/, const TrackInfo& info, const std::string& uri, const std::string& /*meta*/) {
        // DirettaSync is configured internally by AudioEngine::play()
        
        // We use a detached thread to check playlist and set next track.
        // This avoids Deadlock between g_playlistMutex and AudioEngine::m_mutex
        std::thread([uri, info](){ 
             PlayerManager& manager = PlayerManager::getInstance();
             std::lock_guard<std::mutex> lock(manager.getPlaylistMutex());
             
             // ⭐ FIX: Synchronize Playlist Index with currently playing track
             // Since AudioEngine auto-advanced, PlayerManager's index might be stale.
             auto& playlist = manager.getPlaylist();
             int foundIndex = -1;
             for (size_t i = 0; i < playlist.size(); ++i) {
                 // [FIX] CUE Support: Match by URI AND StartTime
                 // Floating point comparison with small epsilon
                 if (playlist[i].path == uri && std::abs(playlist[i].startTime - info.startTime) < 0.1) {
                     foundIndex = static_cast<int>(i);
                     break;
                 }
             }

             if (foundIndex != -1) {
                 // Update global index to match reality
                 manager.setPlaylistIndex(foundIndex);
                 if (g_verbose) std::cout << "[Daemon] Sync: Playlist Index updated to " << foundIndex << std::endl;
                 
                 // ⭐ FIX: Set next track after updating playlist index
                 // This ensures that the next track is always set when switching to a new track
                 UpdateNextTrack();
             }
        }).detach();
    });
    
    // ⭐ FIX: Set track end callback to handle auto next track
    manager.getEngine().setTrackEndCallback([]() {
        std::thread([](){ 
             PlayerManager& manager = PlayerManager::getInstance();
             std::lock_guard<std::mutex> lock(manager.getPlaylistMutex());
             
             // Get next track index
            int nextIdx = GetNextIndex(true);
            if (nextIdx >= 0 && nextIdx < (int)manager.getPlaylist().size()) {
                std::cout << "[Daemon] Track ended: Playing next track " << nextIdx << ": " << manager.getPlaylist()[nextIdx].path << std::endl;
                
                // Update playlist index BEFORE playing next track
                // This ensures GetNextIndex() works correctly for subsequent track changes
                manager.setPlaylistIndex(nextIdx);
                
                // Play next track
                manager.getEngine().playTrack(manager.getPlaylist()[nextIdx]);
            } else {
                std::cout << "[Daemon] Track ended: No more tracks" << std::endl;
            }
        }).detach();
    });

    // 3. Main Loop
    // We used to rely on `engine.process()` loop.
    std::cout << "[Daemon] Entering main loop..." << std::endl;
    
    // Create a dummy format for initial open? No, wait for a track.
    
    while (manager.isRunning()) {
        // Run audio processing
        // This process() should be called at the appropriate rate based on sample rate
        
        if (manager.getEngine().getState() == AudioEngine::State::PLAYING) {
             // Process audio data
             // 8192 samples is quite large (~170ms at 48k), maybe too large for low latency?
             // But existing code used it.
             // ⭐ FIX: Adaptive loop timing based on buffer level
             
             DirettaSync* sync = manager.getEngine().getDirettaSync();
             float bufferLevel = 0.0f;
             if (sync) {
                 bufferLevel = sync->getBufferLevel();
             }
             
             // Adaptive logic:
             // - If buffer full (>50%): Sleep 10ms to save CPU
             // - If buffer low (<50%): Process immediately (little/no sleep)
             // - If buffer critical (<10%): Process repeatedly to catch up
             
             if (bufferLevel > 0.5f) {
                 // Healthy buffer, throttle
                 manager.getEngine().process(8192);
                 std::this_thread::sleep_for(std::chrono::milliseconds(10));
             } else {
                 // Need to fill buffer
                 bool dataProduced = manager.getEngine().process(8192);
                 
                 // If no data (e.g. end of file or decoding slow), sleep briefly to avoid spinloop
                 if (!dataProduced) {
                     std::this_thread::sleep_for(std::chrono::milliseconds(5));
                 } else {
                     // If critical, don't sleep at all to catch up
                     if (bufferLevel > 0.1f) {
                          // Standard filling, minimal sleep
                          std::this_thread::sleep_for(std::chrono::milliseconds(1));
                     }
                     // If < 0.1f, loop immediately (0ms sleep)
                 }
             }
        } else {
             // Sleep longer when not playing
             std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    
    std::cout << "[Daemon] Stopping..." << std::endl;
    ipc.stop();
    // manager.getOutput().stop();
    // manager.getOutput().close(true);
    
    return 0;
}

// ============================================================================
// Main Entry
// ============================================================================
int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // Check for "-c" argument to trigger Client Mode
    // Also support classic arguments for backward compat (if needed) but Priority is IPC.
    bool clientMode = false;
    std::vector<std::string> clientArgs;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-c") {
            clientMode = true;
            // Collect remaining args as command
            for (int j = i + 1; j < argc; ++j) {
                clientArgs.push_back(argv[j]);
            }
            break;
        }
    }

    if (clientMode) {
        return ClientMain(clientArgs);
    } else {
        return DaemonMain();
    }
}
