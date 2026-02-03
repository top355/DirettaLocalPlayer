#include "CueParser.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <cmath>

static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n\"");
    if (std::string::npos == first) return "";
    size_t last = str.find_last_not_of(" \t\r\n\"");
    return str.substr(first, (last - first + 1));
}

static double parseTime(const std::string& timeStr) {
    // MM:SS:FF
    int m = 0, s = 0, f = 0;
    sscanf(timeStr.c_str(), "%d:%d:%d", &m, &s, &f);
    return m * 60.0 + s + (f / 75.0);
}

CueSheet CueParser::parse(const std::string& filePath) {
    CueSheet sheet;
    std::ifstream file(filePath);
    if (!file.is_open()) return sheet;

    std::string line;
    std::string currentFile;
    std::string currentFileType;
    std::string globalPerformer;
    std::string globalTitle;
    
    CueTrack* currentTrack = nullptr;

    int absIndex = 0;

    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string cmd;
        ss >> cmd;
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);

        if (cmd == "PERFORMER") {
            std::string val;
            std::getline(ss, val);
            val = trim(val);
            if (currentTrack) currentTrack->performer = val;
            else globalPerformer = val;
        } else if (cmd == "TITLE") {
            std::string val;
            std::getline(ss, val);
            val = trim(val);
            if (currentTrack) currentTrack->title = val;
            else globalTitle = val;
        } else if (cmd == "FILE") {
            // FILE "filename" TYPE
            std::string rest;
            std::getline(ss, rest);
            size_t lastQuote = rest.find_last_of('"');
            if (lastQuote != std::string::npos) {
                currentFile = trim(rest.substr(0, lastQuote + 1)); // Include quotes to handle correctly in trim
                currentFileType = trim(rest.substr(lastQuote + 1));
            } else {
                currentFile = trim(rest);
            }
        } else if (cmd == "TRACK") {
            int idx;
            std::string type;
            ss >> idx >> type;
            
            CueTrack track;
            track.index = idx;
            track.file = currentFile;
            track.fileType = currentFileType;
            track.performer = globalPerformer; // Inherit global
            track.absoluteIndex = absIndex++;
            sheet.tracks.push_back(track);
            currentTrack = &sheet.tracks.back();
        } else if (cmd == "INDEX") {
            int idx;
            std::string timeStr;
            ss >> idx >> timeStr;
            if (currentTrack && idx == 1) { // We only care about INDEX 01 for StartTime
                currentTrack->startTime = parseTime(timeStr);
            }
        }
    }

    sheet.title = globalTitle;
    sheet.performer = globalPerformer;

    // Calculate durations
    for (size_t i = 0; i < sheet.tracks.size(); ++i) {
        if (i < sheet.tracks.size() - 1) {
            // If next track is in same file
            if (sheet.tracks[i].file == sheet.tracks[i+1].file) {
                 sheet.tracks[i].duration = sheet.tracks[i+1].startTime - sheet.tracks[i].startTime;
            }
        }
    }

    return sheet;
}
