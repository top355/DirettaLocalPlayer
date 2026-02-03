#ifndef PLAYER_MANAGER_H
#define PLAYER_MANAGER_H

#include "AudioEngine.h"
// #include "DirettaOutput.h" // REMOVED

#include <vector>
#include <atomic>
#include <mutex>

class PlayerManager {
public:
    // Singleton instance getter
    static PlayerManager& getInstance() {
        static PlayerManager instance;
        return instance;
    }

    // Delete copy and move constructors to ensure singleton
    PlayerManager(const PlayerManager&) = delete;
    PlayerManager& operator=(const PlayerManager&) = delete;
    PlayerManager(PlayerManager&&) = delete;
    PlayerManager& operator=(PlayerManager&&) = delete;

    // Global state getters and setters
    bool isVerbose() const { return m_verbose; }
    void setVerbose(bool verbose) { m_verbose = verbose; }

    bool isRunning() const { return m_running.load(); }
    void setRunning(bool running) { m_running.store(running); }

    bool isDaemonMode() const { return m_daemonMode.load(); }
    void setDaemonMode(bool daemonMode) { m_daemonMode.store(daemonMode); }

    // Engine and Output getters
    AudioEngine& getEngine() { return m_engine; }

    // DirettaOutput& getOutput() { return m_output; } // REMOVED


    // Playlist management
    std::vector<TrackInfo>& getPlaylist() { return m_playlist; }
    const std::vector<TrackInfo>& getPlaylist() const { return m_playlist; }
    std::mutex& getPlaylistMutex() { return m_playlistMutex; }

    int getPlaylistIndex() const { return m_playlistIndex; }
    void setPlaylistIndex(int index) { m_playlistIndex = index; }

    // Shuffle list management
    std::vector<int>& getShuffleList() { return m_shuffleList; }
    const std::vector<int>& getShuffleList() const { return m_shuffleList; }

private:
    // Private constructor for singleton
    PlayerManager() = default;

    // Global state
    bool m_verbose = false; // Disable for production
    std::atomic<bool> m_running{true};
    std::atomic<bool> m_daemonMode{false};


    AudioEngine m_engine;

    // DirettaOutput m_output; // REMOVED


    // Playlist
    std::vector<TrackInfo> m_playlist;
    std::vector<int> m_shuffleList; // [Feature] Shuffle History
    std::mutex m_playlistMutex;
    int m_playlistIndex = 0;
};

#endif // PLAYER_MANAGER_H
