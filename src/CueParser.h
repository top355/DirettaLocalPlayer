#pragma once
#include <string>
#include <vector>

struct CueTrack {
    int index;
    std::string title;
    std::string performer;
    std::string file;
    std::string fileType;
    double startTime = 0.0;
    double duration = -1.0; // -1 means until end of file
    int absoluteIndex = 0; // Global index in the CUE sheet
};

struct CueSheet {
    std::string title;
    std::string performer;
    std::vector<CueTrack> tracks;
};

class CueParser {
public:
    static CueSheet parse(const std::string& filePath);
};
