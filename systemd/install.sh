#!/bin/bash
# Diretta Local Player (Beta11 Optimized) - Linux Installation Script
#
# This script installs Diretta Local Player as a systemd service on Linux.
# Optimized for multi-architecture support (x86_64/ARM64) and enhanced memory operations.
# No configuration file is required.

set -e

# Detect script location and find binary
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Explicitly check for CMakeLists.txt in the detected directory first
echo "📋 Initial PROJECT_ROOT: $PROJECT_ROOT"
echo "📋 Checking for CMakeLists.txt: $(if [ -f "$PROJECT_ROOT/CMakeLists.txt" ]; then echo "✓ Found"; else echo "✗ Not found"; fi)"

# Force PROJECT_ROOT to be the directory containing the systemd folder
# This ensures we're in the correct project structure
if [[ "$PROJECT_ROOT" == *"systemd" ]]; then
    PROJECT_ROOT="$(dirname "$PROJECT_ROOT")"
fi

echo "📋 Updated PROJECT_ROOT: $PROJECT_ROOT"
echo "📋 Checking for CMakeLists.txt: $(if [ -f "$PROJECT_ROOT/CMakeLists.txt" ]; then echo "✓ Found"; else echo "✗ Not found"; fi)"

# Global Configuration
INSTALL_DIR="/opt/diretta-local-player"
SERVICE_NAME="diretta-local-player"
BINARY_NAME="DirettaLocalPlayer"
WEB_CONTROLLER_DIR="$PROJECT_ROOT/WebController"
WEB_INSTALL_DIR="$INSTALL_DIR/web"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Function to detect Linux distribution
detect_distro() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        echo "$ID"
    elif [ -f /etc/debian_version ]; then
        echo "debian"
    elif [ -f /etc/redhat-release ]; then
        echo "rhel"
    else
        echo "unknown"
    fi
}

# Function to check if we have sudo privileges
check_sudo() {
    if [ "$EUID" -eq 0 ]; then
        return 0  # Already root
    elif command -v sudo > /dev/null 2>&1; then
        # Check if sudo is available without password
        if sudo -n true 2>/dev/null; then
            return 0
        else
            return 1
        fi
    else
        return 2  # sudo not available
    fi
}

# Check command existence
check_command() {
    local cmd=$1
    if command -v "$cmd" > /dev/null 2>&1; then
        return 0
    else
        return 1
    fi
}

# Try to find the binary in common build directories
find_binary() {
    BINARY_PATHS=(
        "$PROJECT_ROOT/build/$BINARY_NAME"
        "$PROJECT_ROOT/build_new/$BINARY_NAME"
        "$PROJECT_ROOT/bin/$BINARY_NAME"
        "$PROJECT_ROOT/../build/$BINARY_NAME"
        "$PROJECT_ROOT/../build_new/$BINARY_NAME"
        "$PROJECT_ROOT/../bin/$BINARY_NAME"
    )

    BINARY_PATH=""
    for path in "${BINARY_PATHS[@]}"; do
        if [ -f "$path" ]; then
            BINARY_PATH="$path"
            break
        fi
    done
}

# Check if binary exists and matches current architecture
check_binary_architecture() {
    if [ -n "$BINARY_PATH" ]; then
        # Detect current architecture
        CURRENT_ARCH=$(uname -m)
        
        echo "📋 Current system architecture: $CURRENT_ARCH"
        
        # Check if 'file' command is available
        if command -v file > /dev/null 2>&1; then
            # Detect binary architecture using file command
            # Use -i for case insensitive and include amd64 as alternative to x86_64
            BINARY_ARCH=$(file "$BINARY_PATH" | grep -i -o -E 'x86_64|amd64|aarch64|riscv64|armv[0-9]+l' || true)
            
            # Convert amd64 to x86_64 for consistency
            if [ "$BINARY_ARCH" = "amd64" ]; then
                BINARY_ARCH="x86_64"
            fi
            
            if [ -z "$BINARY_ARCH" ]; then
                echo "⚠ Could not determine binary architecture, recompiling to be safe..."
                BINARY_PATH=""
            elif [ "$BINARY_ARCH" != "$CURRENT_ARCH" ]; then
                echo "⚠ Binary architecture mismatch! Current: $CURRENT_ARCH, Binary: $BINARY_ARCH"
                echo "⚠ Starting recompilation for correct architecture..."
                BINARY_PATH=""
            else
                echo "✓ Binary found with matching architecture: $BINARY_ARCH"
                echo ""
            fi
        else
            echo "⚠ 'file' command not found, recompiling to ensure compatibility..."
            BINARY_PATH=""
        fi
    fi
}

# Check and install required compilation tools
check_and_install_compilation_tools() {
    echo "🔍 Checking compilation tools..."
    
    # Check all required tools at once
    required_tools=(cmake make gcc g++ git pkg-config)
    optional_tools=(node python3 python)
    missing_tools=()
    
    for tool in "${required_tools[@]}"; do
        if ! check_command "$tool"; then
            missing_tools+=($tool)
        fi
    done
    
    # Check optional tools for web controller
    has_web_tool=false
    for tool in "${optional_tools[@]}"; do
        if check_command "$tool"; then
            has_web_tool=true
            break
        fi
    done
    
    if [ "$has_web_tool" = false ]; then
        echo "⚠ Warning: No web server tools found (Node.js or Python). Web controller may not work properly."
        echo "   Please install Node.js or Python to enable web controller functionality."
        echo ""
    fi
    
    if [ ${#missing_tools[@]} -gt 0 ]; then
        echo "❌ Missing required tools: ${missing_tools[*]}"
        echo ""
        echo "🔄 Attempting to automatically install required compilation tools..."
        echo ""
        
        # Detect distribution
        DISTRO=$(detect_distro)
        echo "📋 Detected distribution: $DISTRO"
        
        # Check sudo privileges
        check_sudo
        SUDO_AVAILABLE=$?
        
        if [ $SUDO_AVAILABLE -eq 0 ]; then
            # We have sudo privileges, proceed with automatic installation
            case $DISTRO in
                debian|ubuntu|dietpi)  # DietPi is debian-based
                    echo "📦 Using apt package manager..."
                    sudo apt update -y
                    # Install required build tools and optional web server tools
                    sudo apt install -y cmake make gcc g++ git pkg-config libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libssl-dev zlib1g-dev
                    ;;
                rhel|centos|fedora)  # RHEL-based distributions
                    echo "📦 Using yum/dnf package manager..."
                    if command -v dnf > /dev/null 2>&1; then
                        sudo dnf install -y cmake make gcc gcc-c++ git pkgconfig ffmpeg-devel openssl-devel zlib-devel
                    else
                        sudo yum install -y cmake make gcc gcc-c++ git pkgconfig ffmpeg-devel openssl-devel zlib-devel
                    fi
                    ;;
                arch|manjaro)  # Arch-based distributions
                    echo "📦 Using pacman package manager..."
                    sudo pacman -Syu --noconfirm cmake make gcc git pkg-config ffmpeg openssl zlib
                    ;;
                *)
                    echo "❌ Unsupported distribution: $DISTRO"
                    echo "Please install the required tools manually, including pkg-config."
                    exit 1
                    ;;
            esac
            
            # Verify installation
            for tool in "${missing_tools[@]}"; do
                if ! check_command "$tool"; then
                    echo ""
                    echo "❌ Failed to install $tool automatically. Please install it manually."
                    exit 1
                fi
            done
            
            echo ""
            echo "✓ Successfully installed all required compilation tools!"
            echo ""
        elif [ $SUDO_AVAILABLE -eq 1 ]; then
            # sudo is available but requires password
            echo "🔑 Need sudo password to install dependencies. Please run the following commands manually:"
            echo ""
            case $DISTRO in
                debian|ubuntu|dietpi)
                    echo "sudo apt update"
                    echo "sudo apt install -y cmake make gcc g++ git pkg-config libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libssl-dev zlib1g-dev"
                    ;;
                rhel|centos|fedora)
                    if command -v dnf > /dev/null 2>&1; then
                        echo "sudo dnf install -y cmake make gcc gcc-c++ git pkgconfig ffmpeg-devel openssl-devel zlib-devel"
                    else
                        echo "sudo yum install -y cmake make gcc gcc-c++ git pkgconfig ffmpeg-devel openssl-devel zlib-devel"
                    fi
                    ;;
                arch|manjaro)
                    echo "sudo pacman -Syu --noconfirm cmake make gcc git pkg-config ffmpeg openssl zlib"
                    ;;
                *)
                    echo "Please install all required tools manually: cmake make gcc g++ git pkg-config"
                    ;;
            esac
            echo ""
            exit 1
        else
            # sudo not available at all
            echo "❌ sudo is not available. Please install the required tools as root."
            exit 1
        fi
    fi
    
    echo "✓ All required compilation tools are available"
    echo ""
}

# Compile the binary
compile_binary() {
    echo "⚠ Binary not found or architecture mismatch! Starting compilation..."
    echo ""
    
    # Check required compilation tools
    check_and_install_compilation_tools
    
    # Create build directory if it doesn't exist
    BUILD_DIR="$PROJECT_ROOT/build"
    
    # Clean up any existing build directory that might have wrong CMakeCache
    if [ -d "$BUILD_DIR" ]; then
        echo "🧹 Cleaning up existing build directory to avoid CMakeCache issues..."
        rm -rf "$BUILD_DIR"
    fi
    
    mkdir -p "$BUILD_DIR"
    
    echo "1. Running CMake configuration..."
    # Ensure we're in the build directory and use the correct source directory
    cd "$BUILD_DIR" && cmake "$PROJECT_ROOT"
    
    echo ""
    echo "2. Compiling Diretta Local Player for $CURRENT_ARCH..."
    cd "$BUILD_DIR" && make -j$(nproc)
    
    echo ""
    echo "✓ Compilation completed successfully for $CURRENT_ARCH!"
    echo ""
    
    # Update binary path after compilation
    BINARY_PATH="$BUILD_DIR/$BINARY_NAME"
    
    if [ ! -f "$BINARY_PATH" ]; then
        echo "❌ Compilation failed. Binary not found."
        exit 1
    fi
}

# Linux systemd installation
install_systemd() {
    echo "════════════════════════════════════════════════════════"
    echo "  Diretta Local Player (Beta11) - Linux Installation"
    echo "════════════════════════════════════════════════════════"
    echo "📂 Project Root: $PROJECT_ROOT"
    echo ""
    
    # Find binary in common locations
    find_binary
    
    # Check binary architecture
    check_binary_architecture
    
    # Compile if binary not found or architecture mismatch
    if [ -z "$BINARY_PATH" ]; then
        compile_binary
    fi
    
    echo "🚀 Installing as systemd service..."
    echo ""
    
    # Check if running as root
    if [ "$EUID" -ne 0 ]; then 
        echo "❌ Please run as root (sudo ./install.sh)"
        exit 1
    fi
    
    SERVICE_FILE="/etc/systemd/system/$SERVICE_NAME.service"
    START_SCRIPT="$INSTALL_DIR/start-player.sh"
    
    # Stop existing service if running
    systemctl stop "$SERVICE_NAME" 2>/dev/null || true
    
    echo "1. Creating installation directory..."
    mkdir -p "$INSTALL_DIR"
    
    echo "2. Copying binary..."
    cp "$BINARY_PATH" "$INSTALL_DIR/"
    chmod +x "$INSTALL_DIR/$BINARY_NAME"
    echo "   ✓ Binary copied to $INSTALL_DIR/$BINARY_NAME"
    
    echo "3. Creating start script..."
    cat > "$START_SCRIPT" << 'EOF'
#!/bin/bash
# Diretta Local Player - Start Script

# Set default values
LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-/opt/diretta-local-player/lib}"
export LD_LIBRARY_PATH

# Set Binary Path for Web Controller (overrides default in JS)
export DIRETTA_BIN_PATH="/opt/diretta-local-player/DirettaLocalPlayer"

# Process IDs to track
PLAYER_PID=""
WEB_PID=""

# Function to terminate all processes
terminate_all() {
    echo "🔴 Stopping Diretta Local Player and web controller..."
    
    if [ -n "$WEB_PID" ]; then
        echo "   Stopping web controller with PID: $WEB_PID"
        kill "$WEB_PID" 2>/dev/null || true
    fi
    
    if [ -n "$PLAYER_PID" ]; then
        echo "   Stopping player with PID: $PLAYER_PID"
        kill "$PLAYER_PID" 2>/dev/null || true
    fi
    
    exit 0
}

# Trap signals to ensure proper cleanup
trap terminate_all SIGINT SIGTERM SIGQUIT

# Start the player
echo "🚀 Starting Diretta Local Player..."
/opt/diretta-local-player/DirettaLocalPlayer &
PLAYER_PID=$!
echo "📝 Player PID: $PLAYER_PID"

# Wait a moment for backend to initialize
sleep 1

# Start web controller in background
echo "🌐 Starting web controller..."
if command -v node > /dev/null 2>&1; then
    # Check if DirettaLocalPlayer_controller.js exists
    if [ -f "/opt/diretta-local-player/web/DirettaLocalPlayer_controller.js" ]; then
        # Start Node.js web server using the controller script
        cd /opt/diretta-local-player/web && node DirettaLocalPlayer_controller.js > /dev/null 2>&1 &
        WEB_PID=$!
        echo "   Web controller started on port 3008 with PID: $WEB_PID"
    else
        # Fallback to simple HTTP server if controller script not found
        python3 -m http.server 3008 --directory /opt/diretta-local-player/web > /dev/null 2>&1 &
        WEB_PID=$!
        echo "   Fallback web server started on port 3008 with PID: $WEB_PID"
    fi
elif command -v python3 > /dev/null 2>&1; then
    # Fallback to Python HTTP server if Node.js not found
    python3 -m http.server 3008 --directory /opt/diretta-local-player/web > /dev/null 2>&1 &
    WEB_PID=$!
    echo "   Fallback web server started on port 3008 with PID: $WEB_PID"
elif command -v python > /dev/null 2>&1; then
    # Fallback to Python 2 HTTP server if Python 3 not found
    python -m SimpleHTTPServer 3008 --directory /opt/diretta-local-player/web > /dev/null 2>&1 &
    WEB_PID=$!
    echo "   Fallback web server started on port 3008 with PID: $WEB_PID"
else
    echo "⚠ Neither Node.js nor Python found, web controller will not be started"
    WEB_PID=""
fi

# Wait for player process
wait $PLAYER_PID

# If we get here, player has exited, clean up web server
terminate_all
EOF
    chmod +x "$START_SCRIPT"
    echo "   ✓ Start script created: $START_SCRIPT"
    
    echo "4. Installing web controller..."
    if [ -d "$WEB_CONTROLLER_DIR" ]; then
        mkdir -p "$WEB_INSTALL_DIR"
        cp -r "$WEB_CONTROLLER_DIR/"* "$WEB_INSTALL_DIR/"
        chmod -R 755 "$WEB_INSTALL_DIR"
        echo "   ✓ Web controller installed to $WEB_INSTALL_DIR"
    else
        echo "   ⚠ WebController directory not found, skipping web controller installation"
        mkdir -p "$WEB_INSTALL_DIR"
    fi
    
    echo "5. Copying default cover image..."
    # Create /data directory if it doesn't exist
    mkdir -p /data
    # Check if default.jpg exists and copy it to /data directory
    if [ -f "$PROJECT_ROOT/default.jpg" ]; then
        cp "$PROJECT_ROOT/default.jpg" /data/
        chmod 644 /data/default.jpg
        echo "   ✓ Default cover image copied to /data/default.jpg"
    else
        echo "   ⚠ default.jpg not found, skipping copy"
    fi
    
    echo "6. Installing systemd service..."
    cat > "$SERVICE_FILE" << 'EOF'
[Unit]
Description=Diretta Local Player (Beta11 Optimized)
Documentation=https://github.com/cometdom/DirettaLocalPlayer
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=root
WorkingDirectory=/opt/diretta-local-player

# Use wrapper script to handle complex command building
ExecStart=/opt/diretta-local-player/start-player.sh

Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal
SyslogIdentifier=diretta-local-player

# Ensure all processes are killed when service stops
KillMode=control-group

# Performance optimizations
Nice=-10

[Install]
WantedBy=multi-user.target
EOF
    echo "   ✓ Service file installed: $SERVICE_FILE"
    
    echo "6. Reloading systemd daemon..."
    systemctl daemon-reload
    
    echo "7. Enabling service (start on boot)..."
    systemctl enable "$SERVICE_NAME.service"
    
    echo ""
    echo "✓ Systemd installation complete!"
    echo ""
    echo "📝 Service file:       $SERVICE_FILE"
    echo "📝 Start script:       $START_SCRIPT"
    echo "📁 Installation dir:   $INSTALL_DIR"
    echo "📁 Web controller:     $WEB_INSTALL_DIR"
    echo ""
    echo "🎯 Next steps:"
    echo ""
    echo "  1. Start the service:"
    echo "     sudo systemctl start $SERVICE_NAME"
    echo ""
    echo "  2. Check status:"
    echo "     sudo systemctl status $SERVICE_NAME"
    echo ""
    echo "  3. View logs:"
    echo "     sudo journalctl -u $SERVICE_NAME -f"
    echo ""
    echo "  4. Stop the service:"
    echo "     sudo systemctl stop $SERVICE_NAME"
    echo ""
    echo "  5. Disable auto-start:"
    echo "     sudo systemctl disable $SERVICE_NAME"
    echo ""
}

# Uninstallation function
uninstall() {
    echo "════════════════════════════════════════════════════════"
    echo "  Diretta Local Player - Uninstallation"
    echo "════════════════════════════════════════════════════════"
    echo ""
    
    echo "🚀 Uninstalling Diretta Local Player..."
    echo ""
    
    # Check if running as root
    if [ "$EUID" -ne 0 ]; then 
        echo "❌ Please run as root (sudo ./install.sh --uninstall)"
        exit 1
    fi
    
    SERVICE_FILE="/etc/systemd/system/$SERVICE_NAME.service"
    
    echo "1. Stopping service..."
    systemctl stop "$SERVICE_NAME" 2>/dev/null || true
    
    echo "2. Disabling service..."
    systemctl disable "$SERVICE_NAME" 2>/dev/null || true
    
    echo "3. Removing service file..."
    rm -f "$SERVICE_FILE" 2>/dev/null || true
    
    echo "4. Reloading systemd daemon..."
    systemctl daemon-reload 2>/dev/null || true
    
    echo "5. Removing installation directory (including web controller)..."
    rm -rf "$INSTALL_DIR" 2>/dev/null || true
    
    echo "6. Removing default cover image..."
    rm -f /data/default.jpg 2>/dev/null || true
    
    echo ""
    echo "✓ Uninstallation complete!"
    echo ""
}

# Show usage
show_usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --uninstall   Uninstall the player"
    echo "  --help        Show this help message"
    echo ""
    echo "If no option is specified, the script will install the player as a systemd service."
    echo ""
}

# Main logic
handle_arguments() {
    case "$1" in
        --uninstall)
            uninstall
            ;;
        --help|-h)
            show_usage
            ;;
        "")
            # No arguments, install as systemd service
            install_systemd
            ;;
        *)
            echo "❌ Unknown option: $1"
            show_usage
            exit 1
            ;;
    esac
}

# Handle command line arguments
handle_arguments "$1"

echo "════════════════════════════════════════════════════════"
echo "  Installation completed successfully!"
echo "════════════════════════════════════════════════════════"
