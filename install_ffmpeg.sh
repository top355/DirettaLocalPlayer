#!/bin/bash
#
# Diretta Local Player - FFmpeg 编译环境安装脚本
#
# 此脚本参照 DirettaRendererUPnP-X 的逻辑编写，用于安装 FFmpeg 及其编译依赖。
# 运行方式: bash install_ffmpeg.sh
#

set -e  # 出错时退出

# =============================================================================
# 配置
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FFMPEG_BUILD_DIR="/tmp/ffmpeg-build"
FFMPEG_HEADERS_DIR="$SCRIPT_DIR/ffmpeg-headers"

# =============================================================================
# 辅助函数
# =============================================================================

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

print_info()    { echo -e "${BLUE}[信息]${NC} $1"; }
print_success() { echo -e "${GREEN}[成功]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[警告]${NC} $1"; }
print_error()   { echo -e "${RED}[错误]${NC} $1"; }
print_header()  { echo -e "\n${CYAN}=== $1 ===${NC}\n"; }

# =============================================================================
# 系统检测
# =============================================================================

detect_system() {
    print_header "系统检测"

    if [ -f /etc/os-release ]; then
        . /etc/os-release
        OS=$ID
        VER=$VERSION_ID
        print_success "检测到系统: $PRETTY_NAME"
    else
        print_error "无法检测 Linux 发行版"
        exit 1
    fi

    ARCH=$(uname -m)
    print_info "硬件架构: $ARCH"
}

# =============================================================================
# 安装基础依赖
# =============================================================================

install_dependencies() {
    print_header "安装编译环境依赖"

    case $OS in
        ubuntu|debian|raspbian|dietpi)
            print_info "使用 APT 安装依赖..."
            sudo apt update
            sudo apt install -y \
                build-essential \
                git \
                wget \
                nasm \
                yasm \
                pkg-config \
                cmake \
                libgnutls28-dev \
                libsoxr-dev \
                libssl-dev \
                zlib1g-dev \
                liblzma-dev \
                libbz2-dev
            ;;
        fedora|rhel|centos)
            print_info "使用 DNF/YUM 安装依赖..."
            local pkg_mgr="dnf"
            command -v dnf > /dev/null 2>&1 || pkg_mgr="yum"
            sudo $pkg_mgr install -y \
                gcc-c++ \
                make \
                git \
                wget \
                nasm \
                yasm \
                pkgconfig \
                cmake \
                gnutls-devel \
                soxr-devel \
                openssl-devel \
                zlib-devel \
                xz-devel \
                bzip2-devel
            ;;
        arch|manjaro)
            print_info "使用 Pacman 安装依赖..."
            sudo pacman -Sy --needed --noconfirm \
                base-devel \
                git \
                wget \
                nasm \
                yasm \
                pkgconf \
                cmake \
                gnutls \
                libsoxr \
                openssl \
                zlib \
                xz \
                bzip2
            ;;
        *)
            print_error "不支持的发行版: $OS，请手动安装编译依赖。"
            exit 1
            ;;
    esac

    print_success "编译依赖安装完成"
}

# =============================================================================
# FFmpeg 配置选项
# =============================================================================

# 最小化音频专用构建配置 (FFmpeg 8.x)
get_ffmpeg_8_minimal_opts() {
    cat <<'OPTS'
--prefix=/usr/local
--enable-shared
--disable-static
--enable-small
--enable-gpl
--enable-version3
--enable-gnutls
--enable-libsoxr
--enable-lto
--enable-stripping
--disable-autodetect
--disable-debug
--disable-logging
--disable-everything
--disable-doc
--disable-programs
--disable-avdevice
--disable-swscale
--disable-bzlib
--disable-iconv
--disable-lzma
--disable-libxcb
--enable-protocol=file,http,https,tcp
--enable-demuxer=flac,wav,dsf,dff,aac,mov
--enable-decoder=flac,alac,pcm_s16le,pcm_s24le,pcm_s32le,dsd_lsbf,dsd_msbf,dsd_lsbf_planar,dsd_msbf_planar,aac
--enable-filter=aresample
--extra-cflags="-ffunction-sections -fdata-sections"
--extra-ldflags="-Wl,--gc-sections -Wl,--as-needed"
OPTS
}

# 完整音频支持配置 (用于 FFmpeg 7.1/5.1.2)
get_ffmpeg_full_opts() {
    cat <<'OPTS'
--prefix=/usr/local
--enable-shared
--disable-static
--enable-gpl
--enable-version3
--enable-gnutls
--enable-libsoxr
--disable-debug
--disable-doc
--disable-autodetect
--enable-network
--enable-protocol=file,http,https,tcp,pipe
--enable-demuxer=flac,wav,dsf,dff,mov,aac,mp3,ogg
--enable-decoder=flac,alac,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,dsd_lsbf,dsd_msbf,dsd_lsbf_planar,dsd_msbf_planar,aac,mp3,vorbis
--enable-parser=aac,flac,mpegaudio
--enable-filter=aresample
OPTS
}

# =============================================================================
# 构建逻辑
# =============================================================================

build_ffmpeg() {
    local version="$1"
    local mode="$2"  # "minimal" 或 "full"

    print_info "正在构建 FFmpeg $version ($mode 模式)..."

    # 清理旧编译环境
    mkdir -p "$FFMPEG_BUILD_DIR"
    cd "$FFMPEG_BUILD_DIR"

    local tarball="ffmpeg-${version}.tar.xz"
    local url="https://ffmpeg.org/releases/$tarball"

    if [ ! -f "$tarball" ]; then
        print_info "下载 FFmpeg ${version}..."
        if ! wget -q --show-progress "$url"; then
            print_error "下载 FFmpeg $version 失败"
            return 1
        fi
    fi

    print_info "解压源码..."
    rm -rf "ffmpeg-${version}"
    tar xf "$tarball"
    cd "ffmpeg-${version}"

    local configure_opts
    if [ "$mode" = "minimal" ]; then
        configure_opts=$(get_ffmpeg_8_minimal_opts | tr '\n' ' ')
    else
        configure_opts=$(get_ffmpeg_full_opts | tr '\n' ' ')
    fi

    print_info "配置构建..."
    eval "./configure $configure_opts"

    print_info "开始编译 (使用 $(nproc) 核心)..."
    make -j$(nproc)

    print_info "安装到 /usr/local..."
    sudo make install
    sudo ldconfig

    print_success "FFmpeg $version 安装完成"
    cd "$SCRIPT_DIR"
}

# =============================================================================
# 头文件兼容性处理 (可选)
# =============================================================================

setup_ffmpeg_headers() {
    local version="$1"
    print_info "配置项目本地 FFmpeg 头文件链接 (ABI 兼容性)..."
    
    mkdir -p "$FFMPEG_HEADERS_DIR"
    cd "$FFMPEG_HEADERS_DIR"
    
    # 指向系统安装的头文件 (Ubuntu/Debian 默认路径)
    # 如果用户是从源码安装到 /usr/local，则指向这里
    local base_inc="/usr/local/include"
    
    if [ -d "$base_inc/libavcodec" ]; then
        rm -f libavformat libavcodec libavutil libswresample
        ln -sf "$base_inc/libavformat" libavformat
        ln -sf "$base_inc/libavcodec" libavcodec
        ln -sf "$base_inc/libavutil" libavutil
        ln -sf "$base_inc/libswresample" libswresample
        print_success "头文件链接已更新至 $base_inc"
    else
        print_warning "未在 $base_inc 找到头文件，请确保 'make install' 已执行。"
    fi
    
    cd "$SCRIPT_DIR"
}

# =============================================================================
# 主流程
# =============================================================================

main() {
    detect_system
    install_dependencies

    print_header "FFmpeg 安装选项"
    echo "1) FFmpeg 8.0.1 最小化音频专用构建 (推荐)"
    echo "2) FFmpeg 7.1 完整音频支持构建"
    echo "3) FFmpeg 5.1.2 稳定版(传统版本)构建"
    echo "4) 仅安装编译依赖 (使用系统 FFmpeg)"
    echo ""
    read -p "请选择 [1-4] (默认 1): " OPTION
    OPTION=${OPTION:-1}

    case $OPTION in
        1)
            build_ffmpeg "8.0.1" "minimal"
            setup_ffmpeg_headers "8.0.1"
            ;;
        2)
            build_ffmpeg "7.1" "full"
            setup_ffmpeg_headers "7.1"
            ;;
        3)
            build_ffmpeg "5.1.2" "full"
            setup_ffmpeg_headers "5.1.2"
            ;;
        4)
            print_info "完成依赖安装，正在退出。"
            ;;
        *)
            print_error "无效选项"
            exit 1
            ;;
    esac

    print_header "安装完成"
    print_info "您可以现在前往 build 目录编译项目:"
    echo "  mkdir -p build && cd build"
    echo "  cmake .."
    echo "  make -j\$(nproc)"
    echo ""
}

main "$@"
