#!/usr/bin/env node
const http = require('http');
const url = require('url');
const path = require('path');
const fs = require('fs');
// [底层] 保留 exec 用于 ffprobe，保留 spawn 用于播放器控制
const { spawn, exec } = require('child_process');
const crypto = require('crypto');

// ==========================================
// [第1区] 配置与常量定义
// ==========================================
const CONFIG = {
    // 服务配置
    service: {
        name: 'diretta-local-player.service',
        port: parseInt(process.env.PORT || 3008, 10)
    },

    // 二进制文件配置
    binary: {
        path: process.env.DIRETTA_BIN_PATH || '/root/direttalocalplayer-beta11/build/DirettaLocalPlayer' // 可通过环境变量配置
    },

    // 目录结构配置
    directories: {
        static: path.resolve(__dirname, 'static'),
        musicRoot: process.env.MUSIC_ROOT || (fs.existsSync(path.join(path.resolve(__dirname, 'static'), 'data')) ? path.join(path.resolve(__dirname, 'static'), 'data') : '/data')
    },

    // 文件路径配置
    files: {
        library: '',
        history: '',
        playbackContext: '',
        coverDir: ''
    },

    // 超时和缓存配置
    timeouts: {
        upload: 10 * 60 * 1000, // 10分钟
        command: 30000, // 30秒
        longRunningCommand: 60000, // 60秒
        metaCache: 24 * 60 * 60 * 1000, // 24小时
        libraryCache: 1 * 60 * 60 * 1000 // 1小时
    },

    // MIME类型映射
    mimeTypes: {
        '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css',
        '.json': 'application/json', '.png': 'image/png', '.jpg': 'image/jpeg',
        '.jpeg': 'image/jpeg', '.gif': 'image/gif', '.wav': 'audio/wav',
        '.mp3': 'audio/mpeg', '.flac': 'audio/flac', '.dsf': 'application/octet-stream',
        '.dff': 'application/octet-stream', '.ape': 'application/octet-stream',
        '.m4a': 'audio/mp4', '.svg': 'image/svg+xml'
    },

    // 封面搜索配置
    coverSearch: {
        subDirs: ['Artwork', 'Scans', 'Covers', 'Art', 'Scanned', '扫描图', '图片', 'Booklet', 'Res'],
        patterns: [
            /^folder$/i,
            /^cover$/i,
            /^front$/i,
            /^default$/i,
            /^封面$/i,
            /^封面\d+$/i,
            /^album$/i,
            /front/i,
            /\s01$/i,
            /_01$/i,
            /^01$/i
        ]
    },

    // 音频文件扩展名配置
    audioExtensions: ['.flac', '.wav', '.mp3', '.dsf', '.m4a', '.aac', '.aiff', '.dff', '.ape', '.iso']
};

// 初始化基于MUSIC_ROOT的文件路径
CONFIG.files.library = path.join(CONFIG.directories.musicRoot, 'library.json');
CONFIG.files.history = path.join(CONFIG.directories.musicRoot, 'history.json');
CONFIG.files.playbackContext = path.join(CONFIG.directories.musicRoot, 'playback_context.json');
CONFIG.files.coverDir = path.join(CONFIG.directories.musicRoot, 'covers');

// 导出配置以便其他模块使用
module.exports = { CONFIG };

// ==========================================
// [第2区] 全局状态管理
// ==========================================
const serverState = {
    host: '127.0.0.1', target: '0', playlist: [], currentIndex: 0, trackStartTime: 0,
    savedProgress: 0, isPlaying: false, duration: 0, cover: '/data/default.jpg', contextName: 'Ready',
    playMode: 0, // 0: 顺序, 1: 单曲, 2: 随机
    ignoreStatusUntil: 0,
    playlistFiles: [],
    audioSpec: { fmt: '', detail: '' },
    isCueMode: false,
    lastKernelPos: -1, // [新] 记录内核物理坐标，用于判断是否真正开始播放
    targetIndex: -1,    // [新] 记录用户想听的索引，直到内核对位成功
    abStatus: { isActive: false, a: -1.0, b: -1.0 }
};

// 上传锁，防止并发竞争
let activeUploadLock = false;



const hostProfiles = {};
let lastActionTime = 0;
let lastPlaybackContext = {};
let activeUploadProcess = null;

// ==========================================// [第3区] 核心工具函数// ==========================================
const sleep = (ms) => new Promise(resolve => setTimeout(resolve, ms));

// 统一路径处理工具函数
function safePathResolve(relativePath) {
    if (!relativePath) return CONFIG.directories.musicRoot;
    // 移除可能的前导斜杠
    const cleanPath = relativePath.startsWith('/') ? relativePath.substring(1) : relativePath;
    // 使用path.resolve确保生成绝对路径，并防止路径遍历攻击
    const resolved = path.resolve(CONFIG.directories.musicRoot, cleanPath);
    // 确保路径在MUSIC_ROOT内
    if (!resolved.startsWith(path.resolve(CONFIG.directories.musicRoot))) {
        throw new Error('Access denied: Path outside music root');
    }
    return resolved;
}

// 获取相对于MUSIC_ROOT的路径
function getRelativePath(absolutePath) {
    if (!absolutePath) return '';
    const musicRoot = path.resolve(CONFIG.directories.musicRoot);
    const resolved = path.resolve(absolutePath);
    if (resolved === musicRoot) return '/';
    return path.relative(musicRoot, resolved);
}

// ==========================================
// [第3区] 命令执行与工具函数
// ==========================================
// 封装的命令执行函数，处理错误并返回默认值
async function executeCommand(args, defaultValue = null, timeout = 30000) {
    try {
        return await runCommand(args, false, timeout);
    } catch (error) {
        console.error(`Command failed (${args.join(' ')}):`, error.message);
        return defaultValue;
    }
}

// 封装的长运行命令执行函数
async function executeLongRunningCommand(args, timeout = CONFIG.timeouts.longRunningCommand) {
    try {
        return await runCommand(args, true, timeout);
    } catch (error) {
        console.error(`Long-running command failed (${args.join(' ')}):`, error.message);
        throw error;
    }
}

async function getHostListId(targetHostStr) {
    try {
        const listOut = await runCommand(['-c', 'list']);
        const lines = listOut.split('\n');
        for (const line of lines) {
            if (line.includes(targetHostStr)) {
                const match = line.match(/HostList\s+(\d+)\s+:/);
                if (match) return match[1];
            }
        }
    } catch (e) { console.error('Get ID Failed:', e); }
    return null;
}

// Monitor Upload Removed - C++ Command is Synchronous
// function monitorUploadCompletion(targetHost, timeoutMs = 60000) { ... }

async function waitForTags(retries = 6) {
    for (let i = 0; i < retries; i++) {
        try {
            const tagOut = await runCommand(['-c', 'tag']);
            const tags = parseTags(tagOut);
            if (tags && tags.length > 0) return tags;
        } catch (e) { }
        await sleep(500);
    }
    return [];
}

let historyCache = {};
async function loadHistory() {
    try {
        if (fs.existsSync(CONFIG.files.history)) {
            historyCache = JSON.parse(await fs.promises.readFile(CONFIG.files.history, 'utf8'));
        }
    } catch (e) { }
}
loadHistory();

async function recordPlay(albumId) {
    if (!albumId) return;
    if (!historyCache[albumId]) historyCache[albumId] = { count: 0, lastPlayed: 0 };
    historyCache[albumId].count++;
    historyCache[albumId].lastPlayed = Date.now();
    try {
        await fs.promises.writeFile(CONFIG.files.history, JSON.stringify(historyCache, null, 2), 'utf8');
    } catch (e) { }
}

const parseTimeSec = (str) => {
    if (!str) return 0;
    const parts = str.trim().split(':').map(p => parseInt(p, 10));
    if (parts.some(isNaN)) return 0;
    if (parts.length === 3) return parts[0] * 3600 + parts[1] * 60 + parts[2];
    if (parts.length === 2) return parts[0] * 60 + parts[1];
    return 0;
};

const parseTags = (output) => {
    const rawTags = output.split('\n').filter(l => { const t = l.trim(); return t && !t.includes('QUIT') && !t.startsWith('-1'); });
    const tagFormatTags = [];
    rawTags.forEach(l => {
        // [修改] 支持解析 Duration 字段 (Tag=Index:StartTime:Title[:Duration])
        // 使用非贪婪匹配 title，并尝试捕获可选的 duration
        const match = l.match(/Tag=(\d+):(\d+):(.+?)(?::(\d+))?$/i);
        if (match) {
            const entry = {
                idx: parseInt(match[1]),
                absoluteStartTime: parseInt(match[2]),
                name: match[3].trim(),
                duration: '00:00'
            };

            // 如果后端提供了 duration (第4个捕获组)，直接使用
            if (match[4]) {
                const durSec = parseInt(match[4], 10);
                if (durSec > 0) {
                    entry.duration = formatDuration(durSec);
                }
            }

            tagFormatTags.push(entry);
        }
    });

    if (tagFormatTags.length > 0) {
        for (let i = 0; i < tagFormatTags.length; i++) {
            const current = tagFormatTags[i];

            // 如果已经从后端解析到了有效时长，则跳过差值计算
            if (current.duration !== '00:00') continue;

            const next = tagFormatTags[i + 1];
            if (next) {
                const durationSec = next.absoluteStartTime - current.absoluteStartTime;
                tagFormatTags[i].duration = formatDuration(durationSec);
            } else if (serverState.duration > current.absoluteStartTime) {
                const durationSec = serverState.duration - current.absoluteStartTime;
                tagFormatTags[i].duration = formatDuration(durationSec);
            } else {
                // 如果没有全局时长参考，保持 00:00 待后续重探测补全
                tagFormatTags[i].duration = '00:00';
            }
        }
        tagFormatTags.forEach(tag => {
            let fmt = 'PCM';
            if (/\.(dsf|dff|iso)$/i.test(tag.name)) fmt = 'DSD';
            else if (/\.flac$/i.test(tag.name)) fmt = 'FLAC';
            else if (/\.wav$/i.test(tag.name)) fmt = 'WAV';
            tag.format = fmt;
        });
        return tagFormatTags;
    }

    return rawTags.map(l => {
        let duration = '00:00'; let name = 'Unknown'; let idx = 0;
        const match = l.trim().match(/^(-?\d+)\s+(\d+)\s+([\d:]+)\s+(.+)$/);
        if (match) { idx = parseInt(match[1]); duration = match[3]; name = match[4].trim(); }
        else { const parts = l.trim().split(/\s+/); idx = parseInt(parts[0]) || 0; duration = parts[2] || '00:00'; name = parts.slice(3).join(' ') || 'Unknown'; }
        let fmt = 'PCM';
        if (/\.(dsf|dff|iso)$/i.test(name)) fmt = 'DSD';
        else if (/\.flac$/i.test(name)) fmt = 'FLAC';
        else if (/\.wav$/i.test(name)) fmt = 'WAV';
        return { idx, duration, name, format: fmt };
    });
};

const enrichTagsWithMeta = async (tags) => {
    if (!tags || tags.length === 0) return tags;
    // 如果没有对应的文件列表，无法补全
    if (!serverState.playlistFiles || serverState.playlistFiles.length === 0) return tags;

    // 只有当 tags 数量与文件数量一致时才尝试补全，避免错位
    // 注意：CUE模式下 tags 数量(分轨) 通常大于 文件数量(整轨)，此时不应通过文件列表补全(CUE已包含时长)
    // 只有非CUE模式（直接播放文件列表）才需要补全
    if (serverState.isCueMode && tags.length !== serverState.playlistFiles.length) return tags;

    const limit = Math.min(tags.length, serverState.playlistFiles.length);
    for (let i = 0; i < limit; i++) {
        // 仅当时长丢失时才补全
        if (tags[i].duration === '00:00' || tags[i].duration === '0:00') {
            const filePath = serverState.playlistFiles[i];
            try {
                const meta = await getAudioMeta(filePath);
                if (meta && meta.duration > 0) {
                    tags[i].duration = formatDuration(meta.duration);
                    // 顺便补全格式信息，如果缺失的话
                    if (meta.fmt && (!tags[i].format || tags[i].format === 'PCM')) {
                        tags[i].format = meta.fmt;
                    }
                }
            } catch (e) { }
        }
    }
    return tags;
};

const formatDuration = (seconds) => {
    if (seconds <= 0) return '00:00';
    const h = Math.floor(seconds / 3600);
    const m = Math.floor((seconds % 3600) / 60);
    const s = Math.floor(seconds % 60);

    if (h > 0) return `${h}:${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
    return `${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`;
};

// 元数据缓存
const metaCache = new Map();
const CACHE_TTL = 24 * 60 * 60 * 1000;
setInterval(() => {
    const now = Date.now();
    for (const [key, value] of metaCache.entries()) {
        if (now - value.timestamp > CACHE_TTL) metaCache.delete(key);
    }
}, 6 * 60 * 60 * 1000);

function parseAudioSpec(probeJson) {
    if (!probeJson || !probeJson.streams || !probeJson.streams[0]) return { fmt: '', detail: '' };
    const s = probeJson.streams[0];
    const codec = s.codec_name.toLowerCase();
    const rate = parseInt(s.sample_rate, 10);
    let bits = s.bits_per_raw_sample ? parseInt(s.bits_per_raw_sample, 10) : 16;

    let fmt = 'UNK'; let detail = '';
    if (codec.includes('dsd')) {
        fmt = 'DSD';
        const realRate = rate * 8;
        const mult = Math.round(realRate / 44100);
        detail = `DSD${mult}`;
    } else {
        if (codec === 'flac') fmt = 'FLAC';
        else if (codec.includes('pcm')) fmt = 'WAV';
        else if (codec === 'mp3') fmt = 'MP3';
        else if (codec === 'alac') fmt = 'ALAC';
        else fmt = codec.toUpperCase();
        const bitMatch = codec.match(/pcm_[sf](\d+)/);
        if (bitMatch) bits = parseInt(bitMatch[1], 10);
        const rateK = parseFloat((rate / 1000).toFixed(1));
        detail = `${bits}bit / ${rateK}kHz`;
    }
    return { fmt, detail };
}

function guessAudioFormatByExtension(ext) {
    let guessedFormat = { fmt: '', detail: '' };
    if (ext === '.dsf' || ext === '.dff' || ext === '.iso') guessedFormat = { fmt: 'DSD', detail: 'DSD64' };
    else if (ext === '.flac') guessedFormat = { fmt: 'FLAC', detail: '24bit / 48kHz' };
    else if (ext === '.wav' || ext === '.aiff') guessedFormat = { fmt: 'WAV', detail: '16bit / 44.1kHz' };
    else if (ext === '.mp3') guessedFormat = { fmt: 'MP3', detail: '320kbps' };
    else if (ext === '.m4a') guessedFormat = { fmt: 'ALAC', detail: 'ALAC format' };
    else if (ext === '.cue') guessedFormat = { fmt: 'CUE', detail: 'CUE format' };

    return guessedFormat;
}

async function getAudioMeta(filePath) {
    const cached = metaCache.get(filePath);
    if (cached && (Date.now() - cached.timestamp) < CACHE_TTL) return cached.data;
    if (!filePath || typeof filePath !== 'string') return { fmt: '', detail: '', duration: 0 };
    try { await fs.promises.access(filePath, fs.constants.R_OK); } catch (e) { return { fmt: '', detail: '', duration: 0 }; }

    if (path.extname(filePath).toLowerCase() === '.cue') {
        try {
            const buffer = await fs.promises.readFile(filePath);
            const content = smartDecode(buffer);
            const match = content.match(/FILE\s+"([^"]+)"/i);
            if (match) {
                const refFile = match[1];
                const absoluteRefPath = path.join(path.dirname(filePath), refFile);
                if (fs.existsSync(absoluteRefPath)) {
                    console.log(`[getAudioMeta] Redirecting probe from CUE to: ${refFile}`);
                    const refMeta = await getAudioMeta(absoluteRefPath);
                    metaCache.set(filePath, { data: refMeta, timestamp: Date.now() });
                    return refMeta;
                }
            }
        } catch (e) {
            console.warn('[getAudioMeta] Failed to parse CUE for redirection:', e);
        }
    }

    const escapedPath = filePath.replace(/"/g, '\\"');

    const cmd = `ffprobe -v error -select_streams a:0 -show_entries stream=sample_rate,bits_per_raw_sample,codec_name:format=duration -of json "${escapedPath}"`;

    return new Promise((resolve) => {
        const timeoutId = setTimeout(() => {
            const result = { fmt: '', detail: '', duration: 0 };
            metaCache.set(filePath, { data: result, timestamp: Date.now() });
            resolve(result);
        }, 5000);

        exec(cmd, { maxBuffer: 1024 * 1024 }, (err, stdout, stderr) => {
            clearTimeout(timeoutId);

            let result = { fmt: '', detail: '', duration: 0 };

            if (!err && stdout) {
                try {
                    const probeData = JSON.parse(stdout);
                    result = parseAudioSpec(probeData);

                    if (probeData.format && probeData.format.duration) {
                        result.duration = parseFloat(probeData.format.duration);
                    }
                } catch (e) {
                    const ext = path.extname(filePath).toLowerCase();
                    result = guessAudioFormatByExtension(ext);
                }
            } else {
                const ext = path.extname(filePath).toLowerCase();
                result = guessAudioFormatByExtension(ext);
            }

            metaCache.set(filePath, { data: result, timestamp: Date.now() });
            resolve(result);
        });
    });
}

async function getSortedFiles(dirPath) {
    try {
        await fs.promises.access(dirPath, fs.constants.R_OK);
        const files = await fs.promises.readdir(dirPath);
        const filteredFiles = files.filter(f => /\.(flac|wav|dsf|dff|mp3)$/i.test(f));
        // [Fix] Use standard ASCII sort to match C++ std::sort behavior
        const sortedFiles = filteredFiles.sort();
        return sortedFiles.map(f => path.join(dirPath, f));
    } catch (e) { return []; }
}

async function loadContext() {
    try {
        await fs.promises.access(CONFIG.files.playbackContext, fs.constants.R_OK);
        const data = await fs.promises.readFile(CONFIG.files.playbackContext, 'utf8');
        lastPlaybackContext = JSON.parse(data);
    } catch (e) { }
}
loadContext();

// [修改] 智能相对跳转工具：简化为直接命令下发
async function smartSeek(host, targetAbsoluteTime) {
    try {
        // 检查 targetAbsoluteTime 是否为相对偏移量（带 +/- 前缀）
        if (typeof targetAbsoluteTime === 'string' && (targetAbsoluteTime.startsWith('+') || targetAbsoluteTime.startsWith('-'))) {
            // 对于相对偏移量，直接发送给 DirettaLocalPlayer
            await runCommand(['-c', 'seek', targetAbsoluteTime]);
        } else {
            // 对于绝对时间，直接发送给 DirettaLocalPlayer
            await runCommand(['-c', 'seek', String(targetAbsoluteTime)]);
            // 更新本地状态
            serverState.savedProgress = parseInt(targetAbsoluteTime);
        }
        serverState.trackStartTime = Date.now() - (serverState.savedProgress * 1000);
        serverState.ignoreStatusUntil = Date.now() + 2000;
    } catch (e) {
        console.error('[SmartSeek] Error:', e);
    }
}

// [核心修复] 自动切歌检测：已移除 (完全信任 C++ 内核的切歌逻辑)
// C++ 内核有 TrackChangeCallback 和 Gapless Preload，无需 JS 轮询切歌
// 只保留一个简单的状态监视，用于前端进度条平滑，不做逻辑控制
setInterval(() => {
    if (serverState.isPlaying) {
        // Nothing to do for CUE logic 
    }
}, 1000);

// [核心修复] 手动切歌执行: 简化为直接调用 next/prev
function advanceTrack(delta) {
    if (delta > 0) runCommand(['-c', 'next']);
    else runCommand(['-c', 'prev']);
}

async function saveContext(updates = {}) {
    try {
        const currentFingerprint = { totalTracks: serverState.playlist ? serverState.playlist.length : 0, activeDuration: serverState.duration || 0 };
        lastPlaybackContext = { ...lastPlaybackContext, ...currentFingerprint, ...updates, timestamp: Date.now() };
        await fs.promises.mkdir(path.dirname(CONFIG.files.playbackContext), { recursive: true });
        await fs.promises.writeFile(CONFIG.files.playbackContext, JSON.stringify(lastPlaybackContext, null, 2), 'utf8');
    } catch (e) { }
}

// [核心修复] 保存快照时，必须包含 isCueMode 状态
function saveProfileSnapshot(host) {
    if (!host) return;
    hostProfiles[host] = {
        playlist: [...serverState.playlist],
        cover: serverState.cover,
        contextName: serverState.contextName,
        currentIndex: serverState.currentIndex,
        savedProgress: serverState.savedProgress,
        duration: serverState.duration,
        timestamp: Date.now(),
        playlistFiles: [...serverState.playlistFiles],
        audioSpec: { ...serverState.audioSpec },

    };
}
function restoreProfileSnapshot(host) {
    if (!host || !hostProfiles[host]) return false;
    const p = hostProfiles[host];

    serverState.playlist = p.playlist || [];
    serverState.cover = p.cover || '/data/default.jpg';
    serverState.contextName = p.contextName || 'Ready';
    serverState.currentIndex = p.currentIndex || 0;
    serverState.savedProgress = p.savedProgress || 0;
    serverState.duration = p.duration || 0;
    serverState.isPlaying = false;

    serverState.playlistFiles = p.playlistFiles || [];
    serverState.audioSpec = p.audioSpec || { fmt: '', detail: '' };



    return true;
}

// ==========================================
// [第4区] 进程与命令执行
// ==========================================
let mpProcess = null;
let libraryCache = null;
let libraryCacheTimestamp = 0;
const LIBRARY_CACHE_TTL = 1 * 60 * 60 * 1000;

const json = (res, obj) => { res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' }); res.end(JSON.stringify(obj)); };
const error = (res, msg) => json(res, { error: msg });
const getMimeType = (filePath) => CONFIG.mimeTypes[path.extname(filePath).toLowerCase()] || 'application/octet-stream';
const isValidHost = (h) => /^[a-zA-Z0-9.:%_,\-\[\]\(\)\s]+$/.test(h);

const smartDecode = (buffer) => {
    const strUtf8 = buffer.toString('utf8');
    if (strUtf8.includes('\uFFFD')) {
        try { const dec = new TextDecoder('gbk'); return dec.decode(buffer); } catch (e) { return strUtf8; }
    }
    return strUtf8;
};

const runCommand = (args, isLongRunning = false, timeout = CONFIG.timeouts.command) => {
    console.log(`[IPC] Executing: ${CONFIG.binary.path} ${args.join(' ')}`);
    return new Promise((resolve, reject) => {
        if (!args || !Array.isArray(args) || args.length === 0) return reject(new Error('Invalid arguments'));

        if (isLongRunning && activeUploadProcess) {
            try { activeUploadProcess.kill('SIGKILL'); } catch (e) { }
            activeUploadProcess = null;
        }

        const child = spawn(CONFIG.binary.path, args, { shell: false });
        if (isLongRunning) activeUploadProcess = child;

        let chunks = [];
        let errChunks = [];
        let timedOut = false;

        const timeoutId = setTimeout(() => {
            timedOut = true;
            try { child.kill('SIGKILL'); if (isLongRunning && activeUploadProcess === child) activeUploadProcess = null; } catch (e) { }
            reject(new Error(`Command timed out: ${args.join(' ')}`));
        }, timeout);

        child.stdout.on('data', (d) => {
            chunks.push(d);
            process.stdout.write(`[Kernel] ${d.toString()}`); // 实时同步到控制台
        });
        child.stderr.on('data', (d) => {
            errChunks.push(d);
            process.stderr.write(`[Kernel-Err] ${d.toString()}`); // 实时同步到控制台
        });

        child.on('close', (code) => {
            clearTimeout(timeoutId);
            if (timedOut) return;
            if (isLongRunning && activeUploadProcess === child) activeUploadProcess = null;

            const stdoutStr = smartDecode(Buffer.concat(chunks));
            const stderrStr = smartDecode(Buffer.concat(errChunks));

            if (stderrStr && code !== 0) reject(new Error(stderrStr));
            else resolve(stdoutStr.trim());
        });

        child.on('error', (err) => {
            clearTimeout(timeoutId);
            if (timedOut) return;
            if (isLongRunning && activeUploadProcess === child) activeUploadProcess = null;
            reject(new Error(`Spawn error: ${err.message}`));
        });
    });
};

function getMpProcess() {
    if (mpProcess && !mpProcess.killed) return mpProcess;
    mpProcess = spawn(CONFIG.binary.path, [], { shell: false });
    mpProcess.stdout.on('data', (data) => {
        const output = data.toString();
        if (output.includes('SelectHost :')) mpProcess.stdin.write('1\n');
    });
    mpProcess.on('exit', () => { mpProcess = null; });
    return mpProcess;
}

process.on('exit', () => { if (mpProcess) mpProcess.kill(); if (activeUploadProcess) activeUploadProcess.kill(); });
process.on('SIGINT', () => process.exit());

async function disconnectOldHost(newHost) {
    const oldHost = serverState.host;
    if (oldHost && oldHost !== newHost) {
        saveProfileSnapshot(oldHost);
        if (mpProcess) { mpProcess.kill(); mpProcess = null; }
        if (activeUploadProcess) { activeUploadProcess.kill(); activeUploadProcess = null; }
        await executeCommand(['-c', 'seek', oldHost, '0']);
        await executeCommand(['-c', 'quit', oldHost]);
        await sleep(200);
    }
    serverState.host = newHost;
}

async function ensureStop() {
    if (activeUploadProcess) { try { activeUploadProcess.kill(); } catch (e) { } activeUploadProcess = null; }
    // 总是发送stop命令到内核，确保当前播放被停止，不依赖于serverState.isPlaying状态
    await executeCommand(['-c', 'stop']);
    serverState.isPlaying = false;
    await sleep(50);
}

// ==========================================
// [第5区] 媒体库与文件系统 (增强版)
// ==========================================
async function readLibrary() {
    const now = Date.now();
    if (libraryCache && (now - libraryCacheTimestamp) < CONFIG.timeouts.libraryCache) return libraryCache;
    try {
        const data = await fs.promises.readFile(CONFIG.files.library, 'utf8');
        libraryCache = JSON.parse(data);
        libraryCacheTimestamp = now;
        return libraryCache;
    } catch { return { items: [] }; }
}
async function writeLibrary(obj) {
    libraryCache = obj;
    libraryCacheTimestamp = Date.now();
    await fs.promises.mkdir(path.dirname(CONFIG.files.library), { recursive: true });
    await fs.promises.writeFile(CONFIG.files.library, JSON.stringify(obj, null, 2), 'utf8');
}

// [增强] 智能封面评分系统
// context: 'root' (专辑根目录) 或 'subdir' (扫描图子目录)
function scoreCoverImage(filename, context) {
    const nameNoExt = path.basename(filename, path.extname(filename)).toLowerCase();

    // 1. 绝对优先：标准命名
    if (/^(folder|cover|front|封面|default|album)$/.test(nameNoExt)) return 100;

    // 2. 高优先级：包含特定关键词
    if (nameNoExt.includes('front') || nameNoExt.includes('cover') || nameNoExt.includes('封面')) return 80;

    // 3. 中优先级：以 01 结尾 (常见于扫描套图的第一张: "xxx 01.jpg")
    if (nameNoExt.endsWith(' 01') || nameNoExt.endsWith('_01') || nameNoExt === '01') return 60;

    // 4. 低优先级：如果是专门的子目录(如"扫描图")，任何图片都可以是封面
    if (context === 'subdir') return 20;

    // 5. 根目录下杂图，尽量不选
    return 0;
}

// [增强] 在指定目录查找最佳封面
async function findCoverInDir(dirPath, context = 'root') {
    try {
        await fs.promises.access(dirPath);
        const dirents = await fs.promises.readdir(dirPath, { withFileTypes: true });

        const allowedExts = ['.jpg', '.png', '.jpeg', '.gif', '.bmp', '.webp'];
        // 过滤出所有图片文件
        const images = dirents
            .filter(d => !d.isDirectory() && !d.name.startsWith('.') && allowedExts.includes(path.extname(d.name).toLowerCase()))
            .map(d => d.name);

        if (images.length === 0) return null;

        // 对所有图片进行评分
        let bestImg = null;
        let maxScore = -1;

        for (const img of images) {
            const score = scoreCoverImage(img, context);
            if (score > maxScore) {
                maxScore = score;
                bestImg = img;
            }
        }

        // 如果在根目录且最高分是0 (说明全是无关杂图)，则放弃，除非图片很少
        if (context === 'root' && maxScore === 0) {
            if (images.length <= 3) return path.join(dirPath, images[0]); // 只有几张图时，盲选第一张
            return null;
        }

        // 如果在子目录(扫描图)且找到了图片，即使是0分(比如 "img555.jpg") 也可以作为候补
        if (bestImg) return path.join(dirPath, bestImg);

    } catch (e) { }
    return null;
}

// [增强] 递归查找子目录
async function findCoverDeep(dirPath) {
    try {
        await fs.promises.access(dirPath);
        const dirents = await fs.promises.readdir(dirPath, { withFileTypes: true });

        // 1. 优先检查预设的 "封面目录" (Artwork, 扫描图...)
        for (const sub of CONFIG.coverSearch.subDirs) {
            const foundDir = dirents.find(d => d.isDirectory() && d.name.toLowerCase() === sub.toLowerCase());
            if (foundDir) {
                // 传入 'subdir' 上下文，表示这是专门放图的地方，放宽匹配标准
                const cover = await findCoverInDir(path.join(dirPath, foundDir.name), 'subdir');
                if (cover) return cover;
            }
        }

        // 2. 如果预设目录没找到，尝试通过 "所有子目录" 查找 (防止用户命名为 'MyScans' 之类)
        // 为了性能，只搜一层深度
        const otherDirs = dirents.filter(d => d.isDirectory() && !d.name.startsWith('.'));
        for (const d of otherDirs) {
            // 跳过已经查过的标准目录
            if (CONFIG.coverSearch.subDirs.some(s => s.toLowerCase() === d.name.toLowerCase())) continue;

            // 在普通子目录查找时，保持严格匹配，避免把 CD1/CD2 里的杂图当封面
            const cover = await findCoverInDir(path.join(dirPath, d.name), 'root');
            if (cover) return cover;
        }

    } catch (e) { }
    return null;
}

async function scanMusic() {
    const items = [];
    const allowedExts = CONFIG.audioExtensions;

    try { await fs.promises.mkdir(CONFIG.files.coverDir, { recursive: true }); } catch (e) { }
    const albumMap = new Map();

    async function walk(currentPath) {
        if (!currentPath.startsWith(path.resolve(CONFIG.directories.musicRoot))) return;
        let dirents;
        try { dirents = await fs.promises.readdir(currentPath, { withFileTypes: true }); } catch (e) { return; }

        // [增强] 封面查找逻辑：当前目录 -> 子目录(扫描图等) -> 父目录
        let foundCoverSrc = await findCoverInDir(currentPath, 'root');
        if (!foundCoverSrc) foundCoverSrc = await findCoverDeep(currentPath);
        if (!foundCoverSrc && currentPath !== CONFIG.directories.musicRoot) foundCoverSrc = await findCoverInDir(path.dirname(currentPath), 'root');



        // 检查目录中是否有CUE文件
        let hasCueFile = false;
        let cueFilePath = '';
        for (const dirent of dirents) {
            if (dirent.isFile()) {
                const ext = path.extname(dirent.name).toLowerCase();
                if (ext === '.cue') {
                    hasCueFile = true;
                    cueFilePath = path.relative(CONFIG.directories.musicRoot, path.join(currentPath, dirent.name));
                    break;
                }
            }
        }

        // 检查是否有音频文件
        const hasAudioFiles = dirents.some(dirent => {
            if (!dirent.isFile()) return false;
            const ext = path.extname(dirent.name).toLowerCase();
            return allowedExts.includes(ext);
        });

        // 如果当前目录有音频文件且尚未处理，则创建专辑条目
        if (hasAudioFiles) {
            const dirRelativePath = path.relative(CONFIG.directories.musicRoot, currentPath);
            const pathParts = dirRelativePath.split(path.sep).filter(Boolean);

            // 改进的专辑识别逻辑：支持多层级文件夹结构
            let album = pathParts[pathParts.length - 1] || 'Unknown Album';
            let artist = 'Unknown Artist';

            if (pathParts.length > 1) {
                // 对于多层级结构，将除最后一部分外的所有部分作为艺术家名
                artist = pathParts.slice(0, -1).join(' / ');
            } else if (pathParts.length === 1) {
                // 只有一层目录，直接作为专辑名
                album = pathParts[0];
            }

            const uniqueStr = `${dirRelativePath}`;
            const id = crypto.createHash('md5').update(uniqueStr).digest('hex');

            if (!albumMap.has(id)) {
                let finalCoverPath = '/data/default.jpg';
                if (foundCoverSrc) {
                    const destName = `${id}${path.extname(foundCoverSrc)}`;
                    const dest = path.join(CONFIG.files.coverDir, destName);
                    try {
                        try { await fs.promises.access(dest); } catch { await fs.promises.copyFile(foundCoverSrc, dest); }
                        finalCoverPath = `/data/covers/${destName}`;
                    } catch (e) { }
                }

                // 获取音频文件格式
                let fmt = 'Unknown';
                for (const dirent of dirents) {
                    if (dirent.isFile()) {
                        const ext = path.extname(dirent.name).toLowerCase();
                        if (allowedExts.includes(ext)) {
                            fmt = ext.replace('.', '').toUpperCase();
                            if (['DSF', 'DFF', 'ISO'].includes(fmt)) fmt = 'DSD';
                            break;
                        }
                    }
                }

                let mtime = 0;
                try { const stats = await fs.promises.stat(currentPath); mtime = stats.mtimeMs; } catch (e) { mtime = Date.now(); }

                const albumItem = {
                    id,
                    uniqueKey: uniqueStr,
                    album,
                    artist,
                    cover: finalCoverPath,
                    path: dirRelativePath,
                    format: fmt,
                    added: mtime,
                    hasCue: hasCueFile, // 恢复CUE文件信息
                    cuePath: hasCueFile ? cueFilePath : '' // 恢复CUE文件路径
                };
                items.push(albumItem);
                albumMap.set(id, albumItem);
            }
        }

        // 继续遍历子目录
        for (const dirent of dirents) {
            const fullPath = path.join(currentPath, dirent.name);
            if (fullPath === CONFIG.files.coverDir || dirent.name.startsWith('.') || dirent.name === '@eaDir') continue;

            if (dirent.isDirectory()) {
                await walk(fullPath);
            }
        }
    }
    try { await walk(CONFIG.directories.musicRoot); } catch (e) { }
    return { items };
}

async function resolveContext(type, value) {
    const result = { cover: '/data/default.jpg', name: 'Unknown', id: null };
    try {
        const lib = await readLibrary();
        let found = null;
        if (type === 'id') {
            found = lib.items.find(i => i.id === value);
            if (found) {
                result.name = found.album;
                if (found.cover) result.cover = found.cover;
                result.id = found.id;
                return result;
            }
        } else {
            // 对于 path 或 dir，尝试反查 Album ID
            let relPath = '';
            let absPath = '';

            if (type === 'path') {
                // value 是文件路径
                absPath = path.isAbsolute(value) ? value : path.resolve(CONFIG.directories.musicRoot, value);
                // 假如文件在 /data/Music/AlbumA/Song.flac，专辑路径通常是 /data/Music/AlbumA
                // 我们尝试匹配专辑路径
                const dirPath = path.dirname(absPath);
                relPath = path.relative(CONFIG.directories.musicRoot, dirPath);
            } else if (type === 'dir') {
                // value 是目录路径
                absPath = path.isAbsolute(value) ? value : path.resolve(CONFIG.directories.musicRoot, value);
                relPath = path.relative(CONFIG.directories.musicRoot, absPath);
            }

            // 尝试精确匹配专辑路径
            // 注意：lib.items[i].path 是相对路径
            if (relPath) {
                found = lib.items.find(i => i.path === relPath || i.path === relPath.replace(/\\/g, '/'));
                if (found) {
                    result.id = found.id;
                    if (found.cover) result.cover = found.cover;
                    result.name = found.album;
                }
            }
        }
    } catch (e) { }

    // 如果还没有找到封面或名字，回退到文件系统嗅探
    if ((!result.name || result.name === 'Unknown') && (type === 'path' || type === 'dir')) {
        let targetDir = safePathResolve(value);
        let isFile = false;
        try { const stats = await fs.promises.stat(targetDir); isFile = stats.isFile(); } catch (e) { return result; }

        if (isFile) {
            result.name = path.basename(path.dirname(targetDir));
            targetDir = path.dirname(targetDir);
        } else {
            result.name = path.basename(targetDir);
        }

        // [增强] 目录浏览模式下的实时封面查找
        if (result.cover === '/data/default.jpg') {
            let foundCover = await findCoverInDir(targetDir, 'root');
            if (!foundCover) foundCover = await findCoverDeep(targetDir);

            if (foundCover) {
                const relative = path.relative(CONFIG.directories.musicRoot, foundCover);
                result.cover = '/data/' + relative.replace(/\\/g, '/');
            }
        }
    }
    return result;
}

// ==========================================
// [第6区] API 路由逻辑
// ==========================================
const routes = {

    'GET /api/hosts': async (req, res) => { const out = await executeCommand(['-c', 'list'], ''); const list = out.split('\n').map(l => l.trim()).filter(Boolean).map(l => ({ value: l.split(/\s+/)[0], label: l })); json(res, { items: list }); },
    'GET /api/targets': async (req, res) => { const out = await executeCommand(['-c', 'target'], ''); const list = out.split('\n').map(l => l.trim()).filter(Boolean).filter(l => l.startsWith('Target')).map(l => { const match = l.match(/Target(\d+)\s+(.+)/); return match ? { value: match[1], label: `${match[2]} (设备 ${match[1]})` } : null; }).filter(Boolean); json(res, { items: list }); },

    'GET /api/lyrics': async (req, res) => {
        const queryObj = url.parse(req.url, true).query;
        const titleParam = queryObj.title ? decodeURIComponent(queryObj.title).trim() : '';

        if (!serverState.playlistFiles || serverState.playlistFiles.length === 0) return json(res, { lyrics: '' });

        let audioPath = null;

        let idx = serverState.currentIndex;
        if (queryObj.trackIdx !== undefined) {
            const parsed = parseInt(queryObj.trackIdx, 10);
            if (!isNaN(parsed)) idx = parsed;
        }

        if (serverState.playlistFiles[idx]) audioPath = serverState.playlistFiles[idx];
        else if (serverState.playlistFiles[0]) audioPath = serverState.playlistFiles[0];

        if (!audioPath) return json(res, { lyrics: '' });

        const dir = path.dirname(audioPath);
        const ext = path.extname(audioPath);
        const audioBaseName = path.basename(audioPath, ext);

        let finalLrcPath = null;

        try {
            const files = await fs.promises.readdir(dir);
            const lrcFiles = files.filter(f => f.toLowerCase().endsWith('.lrc'));

            if (titleParam && lrcFiles.length > 0) {
                const titleLower = titleParam.toLowerCase();
                const titleMatch = lrcFiles.find(lrc => {
                    const lrcName = path.basename(lrc, '.lrc').toLowerCase();
                    return lrcName.includes(titleLower);
                });

                if (titleMatch) {
                    finalLrcPath = path.join(dir, titleMatch);
                }
            }

            if (!finalLrcPath) {
                const exactName = audioBaseName + '.lrc';
                const exactMatch = lrcFiles.find(f => f.toLowerCase() === exactName.toLowerCase());
                if (exactMatch) {
                    finalLrcPath = path.join(dir, exactMatch);
                }
            }

            if (!finalLrcPath) {
                const audioNameLower = audioBaseName.toLowerCase();
                const matchedFile = lrcFiles.find(lrcName => {
                    const lrcBase = path.basename(lrcName, '.lrc').toLowerCase();
                    const keywords = lrcBase.split(/[\s\-\.\(\)]+/).filter(k => k.trim().length > 0);
                    if (keywords.length === 0) return false;
                    return keywords.every(keyword => audioNameLower.includes(keyword));
                });
                if (matchedFile) finalLrcPath = path.join(dir, matchedFile);
            }

            if (finalLrcPath) {
                const buffer = await fs.promises.readFile(finalLrcPath);
                const text = smartDecode(buffer);
                json(res, { lyrics: text });
            } else {
                json(res, { lyrics: '' });
            }
        } catch (e) {
            json(res, { lyrics: '' });
        }
    },

    'GET /api/library/albums': async (req, res) => {
        const { q = '', format = '', offset = 0, limit = 20, sort = 'added', order = 'desc', prefix = '' } = url.parse(req.url, true).query;
        const lib = await readLibrary();
        let items = lib.items.filter(it => {
            if (q) {
                const searchTokens = q.toLowerCase().split(/\s+/).filter(Boolean);
                const targetText = `${it.artist} ${it.album}`.toLowerCase();
                if (!searchTokens.every(token => targetText.includes(token))) return false;
            }
            if (format && format !== 'ALL' && it.format !== format) return false;
            return true;
        });
        if (prefix) {
            const p = prefix.toUpperCase();
            items = items.filter(it => {
                const target = (sort === 'artist') ? it.artist : it.album;
                const firstChar = target.charAt(0).toUpperCase();
                if (p === '#') { return !/^[A-Z]/.test(firstChar); }
                return firstChar === p;
            });
        }
        const isAsc = order === 'asc';
        if (sort === 'added') items.sort((a, b) => isAsc ? (a.added - b.added) : (b.added - a.added));
        else if (sort === 'name') items.sort((a, b) => isAsc ? a.album.localeCompare(b.album, undefined, { numeric: true, sensitivity: 'base' }) : b.album.localeCompare(a.album, undefined, { numeric: true, sensitivity: 'base' }));
        else if (sort === 'artist') items.sort((a, b) => isAsc ? a.artist.localeCompare(b.artist, undefined, { numeric: true, sensitivity: 'base' }) : b.artist.localeCompare(a.artist, undefined, { numeric: true, sensitivity: 'base' }));
        json(res, { total: items.length, items: items.slice(Number(offset), Number(offset) + Number(limit)) });
    },

    'POST /api/library/scan': async (req, res) => { try { const libData = await scanMusic(); await writeLibrary(libData); json(res, { status: 'Success', total: libData.items.length }); } catch (e) { error(res, e.message); } },

    'GET /api/library/recommend': async (req, res) => {
        const lib = await readLibrary();
        const all = lib.items || [];
        const limit = 20;
        const latest = [...all].sort((a, b) => (b.added || 0) - (a.added || 0)).slice(0, limit);
        const today = new Date().toDateString();
        let seed = 0; for (let i = 0; i < today.length; i++) seed += today.charCodeAt(i);
        const daily = all.filter(x => {
            const xSeed = x.id.charCodeAt(0) + x.id.charCodeAt(x.id.length - 1);
            return ((seed + xSeed) % 100) < 15;
        }).slice(0, limit);
        if (daily.length < limit) {
            const diff = limit - daily.length;
            const remain = all.filter(x => !daily.includes(x)).slice(0, diff);
            daily.push(...remain);
        }
        const recent = Object.keys(historyCache).sort((a, b) => historyCache[b].lastPlayed - historyCache[a].lastPlayed).map(id => all.find(x => x.id === id)).filter(x => x).slice(0, limit);
        const most = Object.keys(historyCache).sort((a, b) => historyCache[b].count - historyCache[a].count).map(id => all.find(x => x.id === id)).filter(x => x).slice(0, limit);
        const random = [...all].sort(() => 0.5 - Math.random()).slice(0, limit);
        json(res, { latest, daily, recent, most, random });
    },

    'POST /api/ab': async (req, res) => {
        try {
            const body = await getBodyJson(req);
            const sub = body.cmd; // 'a', 'b', 'toggle', 'clear'
            if (!sub) return error(res, 'Missing cmd');

            const result = await runCommand(['-c', 'ab', sub]);
            json(res, { status: 'success', message: result });
        } catch (e) {
            error(res, e.message);
        }
    },

    'POST /api/ab': async (req, res) => {
        try {
            const body = await getBodyJson(req);
            const sub = body.cmd;
            if (!sub) return error(res, 'Missing cmd');

            // Support special combined command for setting B and Toggling at once
            if (sub === 'b_toggle') {
                await runCommand(['-c', 'ab', 'b']);
                const result = await runCommand(['-c', 'ab', 'toggle']);
                return json(res, { status: 'success', message: result });
            }

            const result = await runCommand(['-c', 'ab', sub]);
            json(res, { status: 'success', message: result });
        } catch (e) { error(res, e.message); }
    },

    'POST /api/set_mode': async (req, res) => {
        try {
            const body = await getBodyJson(req);
            const mode = body.mode !== undefined ? parseInt(body.mode, 10) : 0;

            // Mode 2: Random (Random=On, Repeat=All)
            // Mode 1: Single Loop (Random=Off, Repeat=One)
            // Mode 0: Sequential (Random=Off, Repeat=Off)

            if (mode === 2) {
                await runCommand(['-c', 'repeat', 'all']);
                await runCommand(['-c', 'random', 'on']);
            } else if (mode === 1) {
                await runCommand(['-c', 'random', 'off']);
                await runCommand(['-c', 'repeat', 'one']);
            } else {
                await runCommand(['-c', 'random', 'off']);
                await runCommand(['-c', 'repeat', 'off']);
            }

            // [Fix] 立即更新本地状态并锁定，防止轮询被旧状态覆盖
            serverState.playMode = mode;
            serverState.ignoreStatusUntil = Date.now() + 2000;

            json(res, { status: 'ok', mode });
        } catch (e) { error(res, e.message); }
    },

    'POST /api/play': async (req, res) => {
        try {
            await runCommand(['-c', 'play']);
            serverState.isPlaying = true;
            serverState.ignoreStatusUntil = Date.now() + 2000;
            lastActionTime = Date.now();
            json(res, { status: 'playing' });
        } catch (e) { error(res, e.message); }
    },

    'POST /api/next': async (req, res) => {
        try {
            await executeCommand(['-c', 'next']);
            serverState.targetIndex = serverState.currentIndex + 1; // 设为下一曲
            serverState.ignoreStatusUntil = Date.now() + 10000; // CUE解析可能很慢，给10s
            lastActionTime = Date.now();
            json(res, { status: 'success' });
        } catch (e) { error(res, e.message); }
    },

    'POST /api/prev': async (req, res) => {
        try {
            await executeCommand(['-c', 'prev']);
            serverState.targetIndex = Math.max(0, serverState.currentIndex - 1);
            serverState.ignoreStatusUntil = Date.now() + 10000;
            lastActionTime = Date.now();
            json(res, { status: 'success' });
        } catch (e) { error(res, e.message); }
    },





    'POST /api/pause': async (req, res) => {
        try {
            await runCommand(['-c', 'pause']);
            serverState.isPlaying = false;
            serverState.ignoreStatusUntil = Date.now() + 2000;
            lastActionTime = Date.now();
            json(res, { status: 'success' });
        } catch (e) { error(res, e.message); }
    },

    'POST /api/stop': async (req, res) => {
        try {
            await runCommand(['-c', 'stop']);
            serverState.isPlaying = false;
            serverState.ignoreStatusUntil = Date.now() + 2000;
            lastActionTime = Date.now();
            json(res, { status: 'success' });
        } catch (e) { error(res, e.message); }
    },

    'GET /api/info': async (req, res) => {
        try {
            const result = await runCommand(['-c', 'info']);
            // Parse the result into JSON format
            const info = {};
            result.split('\n').forEach(line => {
                const parts = line.split('=');
                if (parts.length === 2) {
                    info[parts[0].trim()] = parts[1].trim();
                }
            });
            json(res, { status: 'success', info });
        } catch (e) {
            error(res, e.message);
        }
    },

    'POST /api/playlist/add': async (req, res) => {
        const { filePath } = await getBodyJson(req);
        try {
            const p = safePathResolve(filePath);
            // [Simplified] Use local host by default
            const host = serverState.host || '127.0.0.1';
            const result = await runCommand(['-c', 'add', p]);
            json(res, { status: 'success', message: result });
        } catch (e) {
            error(res, e.message);
        }
    },

    'POST /api/playlist/remove': async (req, res) => {
        const { index } = await getBodyJson(req);
        try {
            const result = await runCommand(['-c', 'remove', String(index)]);
            json(res, { status: 'success', message: result });
        } catch (e) {
            error(res, e.message);
        }
    },

    'POST /api/playlist/clear': async (req, res) => {
        try {
            const result = await runCommand(['-c', 'clear']);
            serverState.playlist = [];
            serverState.playlistFiles = [];
            serverState.isPlaying = false;
            serverState.savedProgress = 0;
            await saveContext({ savedProgress: serverState.savedProgress });
            json(res, { status: 'success', message: result });
        } catch (e) {
            error(res, e.message);
        }
    },



    'POST /api/connect': async (req, res) => {
        const { host, target, initialIndex, seamless } = await getBodyJson(req);
        // [Simplified] Assume context is always valid for local player
        try {
            const previousHost = serverState.host;
            if (previousHost && previousHost !== host) { await disconnectOldHost(host); }
            serverState.host = host || '127.0.0.1';
            serverState.target = target || '0';
            // [Fix] Do NOT kill mpProcess here. This resets internal state (m_targetIndex).
            // Let getMpProcess() handle auto-restart if crashed.
            getMpProcess();

            // [Fix] User requested full state initialization on connect (Force Clear)
            // This prevents crashes caused by stale/invalid playlist state after restart
            await executeCommand(['-c', 'clear']); // Clear backend state

            // Reset local state to defaults
            serverState.playlist = [];
            serverState.cover = '/data/default.jpg';
            serverState.contextName = 'Ready';
            serverState.currentIndex = 0;
            serverState.savedProgress = 0;
            serverState.duration = 0;
            serverState.playlistFiles = [];
            serverState.isPlaying = false;

            // Specifies Target First (This initializes output)
            await executeCommand(['-c', 'connect', serverState.target]);

            // No playback start (seektag) or restore (seek)
            // User must manually select music to play

            lastActionTime = Date.now();
            saveProfileSnapshot(host); getMpProcess(); json(res, { status: 'success', mode: 'full_connect' });
        } catch (e) { error(res, e.message); }
    },

    'POST /api/play_album': async (req, res) => {
        if (activeUploadLock) return error(res, 'Another upload is in progress', 429);
        activeUploadLock = true;

        const { id } = await getBodyJson(req);
        try {
            await ensureStop();
            const lib = await readLibrary();
            const album = lib.items.find(item => item.id === id);
            if (!album) throw new Error(`Album not found`);
            recordPlay(id);
            const albumDir = path.join(CONFIG.directories.musicRoot, album.path);
            serverState.cover = album.cover;
            serverState.contextName = album.album;
            await saveContext({ type: 'album', id, savedIndex: 0, savedCover: serverState.cover, savedContextName: serverState.contextName });

            serverState.currentIndex = 0;

            // 扫描目录下的所有有效文件 (CUE 和 音频)
            let validFiles = [];
            try {
                // 读取完整目录，不依赖数据库的 cuePath 字段，实现实时混合
                const dirEntries = await fs.promises.readdir(albumDir, { withFileTypes: true });
                // [修复] 忽略系统自动生成的 .utf8.cue 临时文件，避免列表重复
                const cues = dirEntries.filter(e => {
                    const name = e.name.toLowerCase();
                    return e.isFile() && name.endsWith('.cue') && !name.endsWith('.utf8.cue');
                }).map(e => e.name).sort();

                // [优化] 解析 CUE 引用的文件，避免重复添加整轨文件 & GBK 转码
                const ignoredAudio = new Set();
                const processedCues = [];

                for (const cueFile of cues) {
                    try {
                        const cuePath = path.join(albumDir, cueFile);
                        const buffer = await fs.promises.readFile(cuePath);
                        let content = new TextDecoder('utf-8').decode(buffer);
                        let isGBK = false;

                        // [编码检测] 比较 UTF-8 和 GBK 的解码错误率
                        const utf8Errors = (content.match(/\uFFFD/g) || []).length;
                        if (utf8Errors > 0) {
                            const gbkContent = new TextDecoder('gbk').decode(buffer);
                            const gbkErrors = (gbkContent.match(/\uFFFD/g) || []).length;
                            if (gbkErrors < utf8Errors) {
                                content = gbkContent;
                                isGBK = true;
                            }
                        }

                        // [转码代理]
                        // 如果是 GBK，或者虽然是 UTF-8 但引用的文件名大小写不对（Windows 遗留问题），都需要修复
                        // 我们扫描 content 里的 FILE 行，检查磁盘上是否存在
                        let modifiedContent = content;
                        let needRewrite = isGBK;

                        const fileMatches = [...content.matchAll(/FILE\s+"([^"]+)"/g)];
                        for (const m of fileMatches) {
                            const refName = m[1];
                            const exactPath = path.join(albumDir, refName);

                            if (!fs.existsSync(exactPath)) {
                                // 精确匹配失败，尝试不区分大小写查找
                                const correctName = dirEntries.find(e => e.name.toLowerCase() === refName.toLowerCase())?.name;
                                if (correctName) {
                                    console.log(`[CUE Fix] Fixing filename case: "${refName}" -> "${correctName}"`);
                                    // 替换内容中的文件名 (全局替换，小心不要误伤)
                                    // 使用 split join 或者 replace，注意正则转义
                                    modifiedContent = modifiedContent.replace(refName, correctName);
                                    needRewrite = true;
                                }
                            }
                        }

                        if (needRewrite) {
                            const tempCueName = cueFile + '.utf8.cue';
                            const tempCuePath = path.join(albumDir, tempCueName);
                            await fs.promises.writeFile(tempCuePath, modifiedContent, 'utf8'); // 使用修改后的内容
                            processedCues.push(tempCueName);
                            console.log(`[CUE Fix] Transcoded/Fixed ${cueFile} to: ${tempCueName}`);
                        } else {
                            processedCues.push(cueFile);
                        }

                        // 解析 CUE 内容以过滤音频
                        // 注意：这里用 modifiedContent 来做过滤，确保准确
                        const matches = [...modifiedContent.matchAll(/FILE\s+"([^"]+)"/g)];
                        matches.forEach(m => ignoredAudio.add(m[1].trim().toLowerCase()));
                    } catch (e) {
                        console.error('Failed to parse/transcode CUE:', cueFile, e);
                        processedCues.push(cueFile);
                    }
                }

                const audioExts = CONFIG.audioExtensions;
                const audios = dirEntries.filter(e => {
                    if (!e.isFile()) return false;
                    const nameLower = e.name.toLowerCase();
                    const isAudio = audioExts.includes(path.extname(nameLower));
                    return isAudio && !ignoredAudio.has(nameLower);
                }).map(e => e.name).sort();

                validFiles = [...processedCues, ...audios].map(name => path.join(albumDir, name));
            } catch (e) {
                console.warn('Album scan failed, falling back to basic scan:', e);
                validFiles = await getSortedFiles(albumDir);
            }

            if (validFiles.length === 0) validFiles = [albumDir];

            serverState.playlistFiles = validFiles;
            serverState.isCueMode = validFiles.some(f => f.toLowerCase().endsWith('.cue'));

            // Audio Spec
            serverState.audioSpec = { fmt: '', detail: '' };
            if (serverState.playlistFiles[0]) {
                try {
                    const meta = await getAudioMeta(serverState.playlistFiles[0]);
                    serverState.audioSpec = meta;
                    if (meta.duration > 0) {
                        serverState.duration = meta.duration;
                        console.log(`[play_album] Syncing duration from meta: ${serverState.duration}s`);
                    }
                } catch (e) { }
            }

            console.log('--- 开始同步上传流程 (混合) ---');
            // 1. 上传第一个文件
            await executeLongRunningCommand(['-c', 'upload', validFiles[0]]);
            // 2. 追加剩余文件
            for (let i = 1; i < validFiles.length; i++) {
                await runCommand(['-c', 'add', validFiles[i]]);
            }
            console.log('--- 上传完成 ---');

            // 直接播放
                await executeCommand(['-c', 'play']);
                serverState.isPlaying = true;
                serverState.savedProgress = 0;
                serverState.trackStartTime = Date.now();
                serverState.targetIndex = 0; // 设置targetIndex，确保状态同步时不会错误暂停
                serverState.ignoreStatusUntil = Date.now() + 10000; // 延长锁定期到10秒，确保播放有足够时间初始化
                lastActionTime = Date.now();

            // 获取曲目列表
            const tags = await waitForTags();
            if (tags && tags.length > 0) {
                serverState.playlist = await enrichTagsWithMeta(tags);
                // 如果有曲目，设置时长
                if (serverState.playlist[0]) {
                    serverState.duration = parseTimeSec(serverState.playlist[0].duration);
                }
            } else {
                serverState.playlist = [];
            }

            saveProfileSnapshot('local'); getMpProcess(); json(res, { status: `Success`, cover: serverState.cover, isCueMode: serverState.isCueMode });
        } catch (e) { error(res, e.message); } finally { activeUploadLock = false; }
    },

    'POST /api/play_file': async (req, res) => {
        if (activeUploadLock) return error(res, 'Another upload is in progress', 429);
        activeUploadLock = true;

        const { filePath } = await getBodyJson(req);
        try {
            await ensureStop();
            let p = filePath;
            p = safePathResolve(p);
            const ctx = await resolveContext('path', p);

            // 如果该路径对应一个已知的专辑ID，记录播放历史
            if (ctx.id) recordPlay(ctx.id);

            // 检测是否为CUE文件
            serverState.isCueMode = path.extname(p).toLowerCase() === '.cue';

            const meta = await getAudioMeta(p);
            serverState.playlist = [];
            serverState.duration = meta.duration || 0;

            serverState.playlistFiles = [p];

            await runCommand(['-c', 'upload', p]);

            serverState.cover = ctx.cover;
            serverState.contextName = ctx.name;
            await saveContext({ type: 'file', filePath, savedIndex: 0, savedCover: serverState.cover, savedContextName: serverState.contextName });

            serverState.currentIndex = 0;
            serverState.audioSpec = meta;

            await sleep(500);

            await runCommand(['-c', 'play']);
            serverState.isPlaying = true;
            serverState.savedProgress = 0;
            serverState.trackStartTime = Date.now();
            lastActionTime = Date.now();

            const tags = await waitForTags();
            if (tags && tags.length > 0) {
                serverState.playlist = tags;
                if (serverState.playlist[0]) {
                    serverState.duration = parseTimeSec(serverState.playlist[0].duration);
                }
            }

            saveProfileSnapshot('local'); getMpProcess(); json(res, { status: 'success', isCueMode: serverState.isCueMode });
        } catch (e) { error(res, e.message); } finally { activeUploadLock = false; }
    },

    'POST /api/upload': async (req, res) => {
        if (activeUploadLock) return error(res, 'Another upload is in progress', 429);
        activeUploadLock = true;

        const { dir } = await getBodyJson(req);
        try {
            await ensureStop();
            let p = safePathResolve(dir);
            const ctx = await resolveContext('dir', p);

            // 如果该路径对应一个已知的专辑ID，记录播放历史
            if (ctx.id) recordPlay(ctx.id);

            // 扫描目录下的所有有效文件 (CUE 和 音频)
            let validFiles = [];
            try {
                const dirEntries = await fs.promises.readdir(p, { withFileTypes: true });
                // [修复] 忽略系统自动生成的 .utf8.cue 临时文件
                const cues = dirEntries.filter(e => {
                    const name = e.name.toLowerCase();
                    return e.isFile() && name.endsWith('.cue') && !name.endsWith('.utf8.cue');
                }).map(e => e.name).sort();

                // [优化] 解析 CUE 引用的文件，避免重复添加整轨文件
                const ignoredAudio = new Set();
                const processedCues = []; // 存储处理后的 CUE 文件路径（可能是原文件，也可能是转码后的临时文件）

                for (const cueFile of cues) {
                    try {
                        const cuePath = path.join(p, cueFile);
                        const buffer = await fs.promises.readFile(cuePath);
                        let content = new TextDecoder('utf-8').decode(buffer);
                        let isGBK = false;

                        // [编码检测] 比较 UTF-8 和 GBK 的解码错误率（替换字符数量）
                        const utf8Errors = (content.match(/\uFFFD/g) || []).length;

                        if (utf8Errors > 0) {
                            const gbkContent = new TextDecoder('gbk').decode(buffer);
                            const gbkErrors = (gbkContent.match(/\uFFFD/g) || []).length;

                            // 如果 GBK 的错误更少，或者显著减少，则认为是 GBK
                            // 注意：有些特殊 ASCII 艺术在 GBK 下也可能被误判，但对于中文文件名，GBK 的错误通常远小于 UTF-8
                            if (gbkErrors < utf8Errors) {
                                content = gbkContent;
                                isGBK = true;
                            }
                        }

                        // [转码代理] 如果是 GBK，生成临时的 UTF-8 CUE 文件供 C++ 读取
                        if (isGBK) {
                            const tempCueName = cueFile + '.utf8.cue'; // 临时文件名
                            const tempCuePath = path.join(p, tempCueName);
                            // 将 UTF-8 内容写入临时文件
                            await fs.promises.writeFile(tempCuePath, content, 'utf8');
                            processedCues.push(tempCueName); // 使用临时文件替代原文件
                            console.log(`[GBK Fix] Transcoded ${cueFile} to UTF-8: ${tempCueName}`);
                        } else {
                            processedCues.push(cueFile); // 直接使用原文件
                        }

                        // 解析 CUE 内容以过滤音频
                        const matches = [...content.matchAll(/FILE\s+"([^"]+)"/g)];
                        matches.forEach(m => ignoredAudio.add(m[1].trim().toLowerCase()));
                    } catch (e) {
                        console.error('Failed to parse/transcode CUE:', cueFile, e);
                        processedCues.push(cueFile); // 出错则回退到原文件
                    }
                }

                // 2. 找 音频 (排除被 CUE 引用的文件)
                const audioExts = CONFIG.audioExtensions;
                const audios = dirEntries.filter(e => {
                    if (!e.isFile()) return false;
                    const nameLower = e.name.toLowerCase();
                    const isAudio = audioExts.includes(path.extname(nameLower));

                    // 检查是否存在于忽略列表中 (全小写比对)
                    return isAudio && !ignoredAudio.has(nameLower);
                }).map(e => e.name).sort();

                // 合并列表 (使用处理后的 processedCues)
                validFiles = [...processedCues, ...audios].map(name => path.join(p, name));
            } catch (e) {
                console.warn('Scan failed:', e);
            }

            if (validFiles.length === 0) {
                // 如果空目录，尝试直接上传目录本身(可能是一个挂载点?)
                validFiles = [p];
            }

            serverState.cover = ctx.cover;
            serverState.contextName = ctx.name;
            await saveContext({ type: 'dir', dir, savedIndex: 0, savedCover: serverState.cover, savedContextName: serverState.contextName });

            serverState.currentIndex = 0;
            serverState.playlistFiles = validFiles; // 保存所有文件路径
            serverState.isCueMode = validFiles.some(f => f.toLowerCase().endsWith('.cue'));

            // Audio Spec (取第一个文件)
            serverState.audioSpec = { fmt: '', detail: '' };
            if (serverState.playlistFiles[0]) {
                try {
                    const meta = await getAudioMeta(serverState.playlistFiles[0]);
                    serverState.audioSpec = meta;
                } catch (e) { }
            }

            // --- 核心上传逻辑 (混合播放序列) ---
            // 1. 上传第一个文件 (清空列表 + 添加)
            await runCommand(['-c', 'upload', validFiles[0]]);

            // 2. 追加剩余文件
            for (let i = 1; i < validFiles.length; i++) {
                await runCommand(['-c', 'add', validFiles[i]]);
            }

            // Auto Play
            await runCommand(['-c', 'play']);
            serverState.isPlaying = true;
            serverState.savedProgress = 0;
            serverState.trackStartTime = Date.now();
            serverState.targetIndex = 0; // 设置targetIndex，确保状态同步时不会错误暂停
            serverState.ignoreStatusUntil = Date.now() + 10000; // 延长锁定期到10秒，确保播放有足够时间初始化
            lastActionTime = Date.now();

            const tags = await waitForTags();
            if (tags && tags.length > 0) {
                serverState.playlist = await enrichTagsWithMeta(tags);
                if (serverState.playlist[0]) {
                    serverState.duration = parseTimeSec(serverState.playlist[0].duration);
                }
            }

            saveProfileSnapshot('local'); getMpProcess(); json(res, { status: 'success', isCueMode: serverState.isCueMode });
        } catch (e) { error(res, e.message); } finally { activeUploadLock = false; }
    },

    'POST /api/quit': async (req, res) => { const { host } = await getBodyJson(req); try { serverState.isPlaying = false; serverState.savedProgress = 0; await saveContext({ savedProgress: serverState.savedProgress }); await runCommand(['-c', 'stop']); if (mpProcess) { mpProcess.kill(); mpProcess = null; } saveProfileSnapshot(host); json(res, { status: 'stopped' }); } catch (e) { error(res, e.message); } },

    'POST /api/togglePlay': async (req, res) => {
        try {
            // 根据当前状态发送正确的命令
            if (serverState.isPlaying) {
                await runCommand(['-c', 'pause']);
                serverState.isPlaying = false;
                serverState.savedProgress = Math.floor((Date.now() - serverState.trackStartTime) / 1000);
                await saveContext({ savedProgress: serverState.savedProgress });
            } else {
                await runCommand(['-c', 'play']);
                serverState.isPlaying = true;
                serverState.trackStartTime = Date.now() - (serverState.savedProgress * 1000);
            }

            lastActionTime = Date.now();
            json(res, { status: 'ok', isPlaying: serverState.isPlaying });
        } catch (e) { error(res, e.message); }
    },

    'POST /api/seek': async (req, res) => {
        const { host, offset } = await getBodyJson(req);
        try {
            const offsetStr = String(offset).trim();
            // 直接将 offsetStr 传递给 smartSeek，保留其原始格式（相对或绝对）
            await smartSeek(host, offsetStr);
            json(res, { status: 'success' });
        } catch (e) { error(res, e.message); }
    },

    'POST /api/seektag': async (req, res) => {
        const { host, tag } = await getBodyJson(req);
        try {
            // 前端发送的是0-based索引，直接使用
            await runCommand(['-c', 'seektag', String(tag)]);
            const tagNum = parseInt(tag);
            if (!isNaN(tagNum)) {
                serverState.targetIndex = tagNum;
                serverState.currentIndex = tagNum;
                serverState.savedProgress = 0;
                serverState.trackStartTime = Date.now();
                serverState.isPlaying = true;
                serverState.ignoreStatusUntil = Date.now() + 10000; // CUE 加载期间锁定
                lastActionTime = Date.now();
                saveContext({ savedIndex: tagNum, savedProgress: 0 });
            }
            json(res, { status: 'success' });
        } catch (e) { error(res, e.message); }
    },

    // [核心修复] 状态同步逻辑分流 
    'POST /api/status': async (req, res) => {
        const { host } = await getBodyJson(req);
        getMpProcess();

        try {
            if (host === serverState.host) {
                let so = '';
                try { so = await runCommand(['-c', 'status']); } catch (e) { }

                const isLocked = Date.now() < serverState.ignoreStatusUntil;

                // 1. 健壮解析：按行分割提取所有键值对
                const lines = so.split(/\r?\n/);
                const statusMap = {};
                lines.forEach(line => {
                    const match = line.match(/^([^=]+)=(.*)$/);
                    if (match) {
                        const key = match[1].trim();
                        const val = match[2].trim();
                        statusMap[key] = val;

                        // ⭐ 解析 AB 状态到 serverState
                        if (!serverState.abStatus) serverState.abStatus = { isActive: false, a: -1.0, b: -1.0 };
                        if (key === 'ABActive') serverState.abStatus.isActive = (val === '1');
                        if (key === 'ABA') serverState.abStatus.a = parseFloat(val);
                        if (key === 'ABB') serverState.abStatus.b = parseFloat(val);

                        // ⭐ Beta10: 解析实时格式信息 (AudioFmt=24bit/48000Hz)
                        if (key === 'AudioFmt') {
                            const parts = val.split('/');
                            if (parts.length === 2) {
                                const bits = parseInt(parts[0], 10);
                                const rate = parseInt(parts[1], 10);

                                let fmt = 'PCM';
                                let detail = '';

                                // DSD Detection (1-bit or high sample rate)
                                if (bits === 1 || rate >= 2822400) {
                                    fmt = 'DSD';
                                    const mult = Math.round(rate / 44100);
                                    detail = `DSD${mult}`;
                                } else {
                                    // PCM
                                    detail = `${bits}bit / ${(rate / 1000).toFixed(1)}kHz`;
                                }
                                serverState.audioSpec = { fmt, detail };
                            }
                        }

                        // ⭐ Beta10: 解析实时元数据
                        if (key === 'ContextName') serverState.contextName = val;
                        // Title could be used if we had a field for it, currently playlist[currentIndex].name is used
                    }
                });

                // 提取坐标
                const realIdx = statusMap['Tag'] !== undefined ? parseInt(statusMap['Tag'], 10) : -1;
                const lastTime = statusMap['LastTime'] !== undefined ? parseInt(statusMap['LastTime'], 10) : 0;
                const startTime = statusMap['StartTime'] !== undefined ? parseInt(statusMap['StartTime'], 10) : 0;
                const duration = statusMap['Duration'] !== undefined ? parseInt(statusMap['Duration'], 10) : 0;

                // ⭐ [核心逻辑] 位移监测：如果检测到内核 LastTime 动了，说明加载完成
                if (lastTime !== serverState.lastKernelPos && lastTime !== 0) {
                    if (serverState.targetIndex !== -1) {
                        console.log(`[Kernel Driven] Playback Detected (LastTime moved): ${serverState.lastKernelPos} -> ${lastTime}. Releasing target selection lock.`);
                        serverState.targetIndex = -1; // 解锁
                    }
                    serverState.lastKernelPos = lastTime;
                }

                // 2. 处理 State (意图优先)
                const kernelState = (statusMap['State'] || '').toLowerCase();
                const isKernelPlaying = (kernelState === 'play');

                if (serverState.targetIndex !== -1) {
                    // 转场期间，无视内核状态，由于用户下达了播放意图，强制维持推算
                    serverState.isPlaying = true;
                } else if (isLocked && !isKernelPlaying && (Date.now() - lastActionTime < 5000)) {
                    // 动作锁定期，保持现状
                    serverState.isPlaying = true;
                } else {
                    serverState.isPlaying = isKernelPlaying;
                }

                // 3. 索引对齐同步 (SSoT)
                if (realIdx !== -1) {
                    // [核心修复] 在 CUE 模式或索引切换时，必须强制同步 Duration，确保进度条显示曲目时长而非整轨时长
                    const oldIndex = serverState.currentIndex;
                    const oldDuration = serverState.duration;
                    if (realIdx !== oldIndex || (serverState.isCueMode && oldDuration !== duration)) {
                        console.log(`[Kernel Driven] State Sync: Index(${oldIndex}->${realIdx}), Duration(${oldDuration}->${duration})`);
                        serverState.currentIndex = realIdx;
                        serverState.duration = duration;
                        // 只有索引真的变了才重置进度
                        if (realIdx !== oldIndex) {
                            serverState.savedProgress = 0;
                            serverState.trackStartTime = Date.now();
                        }
                        saveProfileSnapshot(host);
                    }
                }

                // 4. 进度推算与物理对齐
                // ⭐ [CUE 修复] C++ 后端的 LastTime 已经是相对于当前音轨的进度
                // 不需要减去 StartTime（StartTime 是音轨在整个 CUE 文件中的绝对起始时间）
                // 修复前：const kernelRelativeProgress = Math.max(0, lastTime - startTime);
                // 这会导致 CUE 模式下计算出负值（如 206 - 2424 = -2218），被截断为 0，造成进度跳回
                const kernelRelativeProgress = Math.max(0, lastTime);

                if (serverState.isPlaying) {
                    // ⭐ [核心加固] 只有在非动作锁定期，且后端索引已确实对齐前端目标时，才允许执行“强纠偏”
                    if (serverState.targetIndex === -1 && !isLocked && realIdx === serverState.currentIndex && Math.abs(serverState.savedProgress - kernelRelativeProgress) > 3) {
                        console.log(`[Kernel Driven] Phase Correct: Local(${serverState.savedProgress}s) -> Kernel(${kernelRelativeProgress}s)`);
                        serverState.savedProgress = kernelRelativeProgress;
                        serverState.trackStartTime = Date.now() - (kernelRelativeProgress * 1000);
                    } else {
                        // 平滑推算阶段：
                        // ⭐ [缓冲修复] 如果内核还没有开始走字 (lastKernelPos 为 -1 或 0)，
                        // 前端不要抢跑 (Extrapolate)，而是原地踏步等待。
                        // 避免出现：前端跑了 4s -> 内核才开始播 0s -> 前端跳回 0s 的尴尬观感。
                        if (kernelRelativeProgress === 0 && (serverState.lastKernelPos === -1 || serverState.lastKernelPos === 0)) {
                            serverState.savedProgress = 0;
                            serverState.trackStartTime = Date.now(); // 持续重置起点，直到内核启动
                        } else {
                            // 只有内核动了，或者是暂停后恢复（pos > 0），才允许推算
                            serverState.savedProgress = Math.floor((Date.now() - serverState.trackStartTime) / 1000);
                        }
                    }
                } else {
                    serverState.savedProgress = kernelRelativeProgress;
                    serverState.trackStartTime = Date.now() - (kernelRelativeProgress * 1000);
                }

                // 防溢出
                if (serverState.duration > 0 && serverState.savedProgress > serverState.duration) serverState.savedProgress = serverState.duration;
                if (serverState.savedProgress < 0) serverState.savedProgress = 0;

                // 4. 首先确保playlistFiles已正确设置
                if (serverState.playlistFiles.length === 0 && serverState.playlist.length > 0) {
                    try {
                        // 对于分轨文件，尝试从contextName获取目录路径
                        if (serverState.contextName) {
                            const contextParts = serverState.contextName.split('/');
                            if (contextParts.length > 1) {
                                // 尝试多种路径解析方式
                                const albumDir = safePathResolve(contextParts.slice(0, -1).join('/'));
                                if (fs.existsSync(albumDir)) {
                                    serverState.playlistFiles = await getSortedFiles(albumDir);
                                }
                            }
                        }
                        // 如果还是没有playlistFiles，尝试从播放列表项的名称中提取文件信息
                        if (serverState.playlistFiles.length === 0 && serverState.contextName) {
                            // 对于分轨文件，contextName可能是目录路径
                            const dirPath = safePathResolve(serverState.contextName);
                            if (fs.existsSync(dirPath)) {
                                serverState.playlistFiles = await getSortedFiles(dirPath);
                            }
                        }
                    } catch (e) {
                        console.warn('[API/status] Failed to set playlistFiles:', e);
                    }
                }

                // 5. 现在获取最新的音频规格信息... 
                const idx = serverState.currentIndex;
                if (serverState.playlistFiles && serverState.playlistFiles[idx]) {
                    const audioFile = serverState.playlistFiles[idx];
                    // 立即更新缓存中的音频规格信息
                    const cached = metaCache.get(audioFile);
                    if (cached && (Date.now() - cached.timestamp) < CACHE_TTL) {
                        serverState.audioSpec = cached.data;
                    } else {
                        // 异步更新缓存
                        getAudioMeta(audioFile).then(meta => {
                            serverState.audioSpec = meta;
                        });
                    }
                } else if (serverState.playlistFiles.length > 0) {
                    // 对于分轨文件，所有轨道都来自同一个音频文件
                    const audioFile = serverState.playlistFiles[0];
                    const cached = metaCache.get(audioFile);
                    if (cached && (Date.now() - cached.timestamp) < CACHE_TTL) {
                        serverState.audioSpec = cached.data;
                    } else {
                        getAudioMeta(audioFile).then(meta => {
                            serverState.audioSpec = meta;
                        });
                    }
                }


                // 将同步后的完整状态返回给前端
                json(res, { statusRaw: so, serverState: buildServerState(so) });
            } else {
                const cached = hostProfiles[host];
                if (cached) { json(res, { serverState: { ...cached, activeHost: serverState.host, isPlaying: false } }); }
                else { json(res, { serverState: { activeHost: serverState.host } }); }
            }
        } catch (e) { error(res, e.message); }
    },

    'GET /api/last_context': async (req, res) => { try { if (fs.existsSync(CONFIG.files.playbackContext)) { const data = await fs.promises.readFile(CONFIG.files.playbackContext, 'utf8'); lastPlaybackContext = JSON.parse(data); json(res, lastPlaybackContext); } else { json(res, {}); } } catch (e) { json(res, {}); } },
    'POST /api/tags': async (req, res) => {
        try {
            const out = await runCommand(['-c', 'tag']);
            let tags = parseTags(out);

            if (tags && tags.length > 0) {
                // [深度修复] 即时重探测机制
                const lastTag = tags[tags.length - 1];
                if (serverState.isCueMode && (lastTag.duration === '00:00' || serverState.duration === 0)) {
                    if (serverState.playlistFiles && serverState.playlistFiles[0]) {
                        console.log('[POST /api/tags] Triggering immediate re-probe for CUE duration...');
                        const meta = await getAudioMeta(serverState.playlistFiles[0]);
                        if (meta && meta.duration > 0) {
                            serverState.duration = meta.duration;
                            const durationSec = serverState.duration - lastTag.absoluteStartTime;
                            lastTag.duration = formatDuration(durationSec);
                            console.log(`[POST /api/tags] Re-probe success: ${serverState.duration}s`);
                        }
                    }
                }
                tags = await enrichTagsWithMeta(tags);
                serverState.playlist = tags;
            } else {
                serverState.playlist = [];
            }
            // 返回 tags 的同时也返回 serverState 的关键元数据，确信 UI 同步
            json(res, {
                tags: serverState.playlist,
                serverState: {
                    duration: serverState.duration,
                    isCueMode: serverState.isCueMode
                }
            });
        } catch (e) { error(res, e.message); }
    },
    'POST /api/admin/clear_db': async (req, res) => { try { if (fs.existsSync(CONFIG.files.library)) await fs.promises.unlink(CONFIG.files.library); if (fs.existsSync(CONFIG.files.coverDir)) { const files = await fs.promises.readdir(CONFIG.files.coverDir); for (const f of files) await fs.promises.unlink(path.join(CONFIG.files.coverDir, f)); } libraryCache = null; json(res, { status: 'success' }); } catch (e) { error(res, e.message); } },
    'POST /api/admin/compact_covers': async (req, res) => { try { if (!fs.existsSync(CONFIG.files.coverDir)) return error(res, 'No covers found'); const files = await fs.promises.readdir(CONFIG.files.coverDir); let count = 0; for (const file of files) { if (!/\.(jpg|jpeg|png)$/i.test(file)) continue; const filePath = path.join(CONFIG.files.coverDir, file); const tempPath = path.join(CONFIG.files.coverDir, `temp_${file}`); await new Promise((resolve) => { const ff = spawn('ffmpeg', ['-i', filePath, '-vf', 'scale=300:300:force_original_aspect_ratio=decrease', '-y', tempPath]); ff.on('close', async (code) => { if (code === 0) { try { await fs.promises.rename(tempPath, filePath); count++; } catch (e) { } } resolve(); }); ff.on('error', () => resolve()); }); } json(res, { status: 'success', processed: count }); } catch (e) { error(res, e.message); } },

    'GET /api/browse': async (req, res) => {
        try {
            const queryPath = url.parse(req.url, true).query.path ? decodeURIComponent(url.parse(req.url, true).query.path) : '';
            let targetPath;
            if (!queryPath || queryPath === '/') {
                targetPath = CONFIG.directories.musicRoot;
            } else {
                if (queryPath.startsWith('/')) {
                    targetPath = safePathResolve(queryPath.slice(1));
                } else {
                    targetPath = safePathResolve(queryPath);
                }
            }

            const musicRoot = path.resolve(CONFIG.directories.musicRoot);
            const resolvedTarget = path.resolve(targetPath);
            if (!resolvedTarget.startsWith(musicRoot)) {
                return error(res, 'Path not found');
            }

            try { await fs.promises.access(resolvedTarget); } catch { return error(res, 'Path not found'); }

            const dirs = [], files = [];
            const entries = await fs.promises.readdir(resolvedTarget, { withFileTypes: true });
            const allowedExts = ['.flac', '.wav', '.mp3', '.dsf', '.m4a', '.aac', '.aiff', '.dff', '.ape', '.iso', '.cue'];

            entries.forEach(d => {
                if (d.name.startsWith('.') || d.name === 'covers') return;
                if (d.isDirectory()) {
                    dirs.push(d.name);
                } else {
                    const ext = path.extname(d.name).toLowerCase();
                    if (allowedExts.includes(ext)) {
                        files.push(d.name);
                    }
                }
            });

            dirs.sort((a, b) => a.toLowerCase().localeCompare(b));
            files.sort((a, b) => a.toLowerCase().localeCompare(b));

            const relativePath = resolvedTarget === musicRoot ? '/' : path.relative(musicRoot, resolvedTarget);
            json(res, {
                current_path: relativePath,
                is_root: (resolvedTarget === musicRoot),
                directories: dirs,
                files
            });
        } catch (e) { error(res, e.message); }
    },

    'GET /': (req, res) => serveStatic(res, 'DirettaLocalPlayer_controller.html'),
    'GET /DirettaLocalPlayer_controller.html': (req, res) => serveStatic(res, 'DirettaLocalPlayer_controller.html')
};


function getBodyJson(req) { return new Promise(resolve => { let b = ''; req.on('data', c => b += c); req.on('end', () => { try { resolve(JSON.parse(b || '{}')); } catch (e) { resolve({}); } }); }); }

function serveStatic(res, file) {
    const fp = path.join(CONFIG.directories.static, file);
    if (fs.existsSync(fp)) {
        res.writeHead(200, { 'Content-Type': getMimeType(fp), 'Cache-Control': 'public, max-age=86400' });
        fs.createReadStream(fp).pipe(res);
    } else { error(res, '404 Not Found'); }
}

const server = http.createServer((req, res) => {
    res.setHeader('Access-Control-Allow-Origin', '*');
    const pathname = url.parse(req.url).pathname;
    if (req.method === 'POST' || req.method === 'GET') { const key = `${req.method} ${pathname}`; if (routes[key]) return routes[key](req, res); }
    if (req.method === 'GET') {
        const decodedPath = decodeURIComponent(pathname);
        if (decodedPath.startsWith('/data/')) {
            const relativePath = decodedPath.replace('/data/', '');
            const sysPath = safePathResolve(relativePath);
            if (fs.existsSync(sysPath) && fs.statSync(sysPath).isFile()) {
                res.writeHead(200, { 'Content-Type': getMimeType(sysPath), 'Cache-Control': 'public, max-age=604800, immutable' });
                fs.createReadStream(sysPath).pipe(res);
                return;
            }
        }
        const f = pathname.substring(1); if (f.includes('..')) { error(res, 'Access Denied'); return; } const safePath = path.normalize(f); const tryPaths = [safePath, 'DirettaLocalPlayer_controller.html']; for (const p of tryPaths) { if (fs.existsSync(path.join(CONFIG.directories.static, p))) return serveStatic(res, p); }
    }
    error(res, 'Not Found');
});

server.on('error', (e) => {
    console.error(`[Error] Server error: ${e.code}: ${e.message}`);
    process.exit(1);
});

// 移除多余的全局定时器，避免干扰路由层面的实时状态同步逻辑
setInterval(() => { if (serverState.host) getMpProcess(); }, 5000);
server.setTimeout(0);
server.listen(CONFIG.service.port, () => { console.log(`MemoryPlay Server Running on ${CONFIG.service.port}, Root: ${CONFIG.directories.musicRoot}`); });

function buildServerState(so) {
    let activeCover = serverState.cover;
    if ((!activeCover || activeCover.includes('default.jpg')) && lastPlaybackContext.savedCover && serverState.contextName === lastPlaybackContext.savedContextName) {
        activeCover = lastPlaybackContext.savedCover; serverState.cover = activeCover;
    }
    return {
        activeHost: serverState.host, activeTarget: serverState.target, playlist: serverState.playlist,
        currentIdx: serverState.currentIndex, isPlaying: serverState.isPlaying, progress: serverState.savedProgress,
        duration: serverState.duration, cover: activeCover, contextName: serverState.contextName,
        playMode: serverState.playMode, isCueMode: serverState.isCueMode,
        abStatus: serverState.abStatus || { isActive: false, a: -1, b: -1 },
        audioFmt: serverState.audioSpec ? serverState.audioSpec.fmt : '',
        audioDetail: serverState.audioSpec ? serverState.audioSpec.detail : ''
    };
}