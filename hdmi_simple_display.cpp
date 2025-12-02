// hdmi_simple_display.cpp - HDMI input display application
// Captures video from V4L2 device and displays it using OpenGL ES
// Supports test pattern fallback when no signal is available

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <linux/videodev2.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

// Platform-specific includes for OpenGL ES / EGL
#ifdef HAVE_EGL
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#else
// Stub types when EGL is not available
typedef unsigned int GLuint;
typedef int GLint;
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
static inline void glGenTextures(int, GLuint*) {}
static inline void glBindTexture(int, GLuint) {}
static inline void glTexParameteri(int, int, int) {}
static inline void glTexImage2D(int, int, int, int, int, int, int, int, const void*) {}
#endif

// STB Image library for loading test pattern images
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// Configuration constants
constexpr int PATTERN_TIMEOUT_MS = 3000;      // Timeout before showing test pattern
constexpr int QBUF_RETRIES = 3;               // Number of QBUF retry attempts
constexpr int STREAM_RESET_RETRIES = 3;       // Number of stream reset attempts
constexpr int DEFAULT_WIDTH = 1920;
constexpr int DEFAULT_HEIGHT = 1080;
constexpr int NUM_BUFFERS = 4;
constexpr int MAX_IMAGE_WIDTH = 8192;         // Maximum test pattern image width
constexpr int MAX_IMAGE_HEIGHT = 8192;        // Maximum test pattern image height

// Global state
static bool g_verbose = false;
static bool g_running = true;

// Logging macros
#define LOG_VERBOSE(fmt, ...)                          \
    do {                                               \
        if (g_verbose) {                               \
            fprintf(stderr, "[VERBOSE] " fmt "\n", ##__VA_ARGS__); \
        }                                              \
    } while (0)

#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) fprintf(stderr, "[INFO] " fmt "\n", ##__VA_ARGS__)

// Structure to hold V4L2 buffer information
struct V4L2Buffer {
    void* start;
    size_t length;
};

// Structure to hold application state
struct AppState {
    // V4L2 state
    int v4l2_fd;
    std::vector<V4L2Buffer> buffers;
    bool streaming;
    int stream_reset_count;
    
    // OpenGL state
    GLuint texFrame;
    GLuint texPattern;
    GLuint program;
    GLint u_showPattern_loc;
    GLint texFrame_loc;
    GLint texPattern_loc;
    
    // Pattern state
    bool haveTestPattern;
    bool showPattern;
    std::chrono::steady_clock::time_point lastFrameTime;
    
    // Video dimensions
    int width;
    int height;
};

// Get the directory containing the executable
static std::string getExecutableDir() {
    char path[4096];
    ssize_t count = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (count > 0) {
        path[count] = '\0';
        std::string fullPath(path);
        size_t lastSlash = fullPath.find_last_of('/');
        if (lastSlash != std::string::npos) {
            return fullPath.substr(0, lastSlash);
        }
    }
    return ".";
}

// Check if a file exists and is readable
static bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Search for test pattern image in various locations
static std::string findTestPatternImage(const std::string& cliPath) {
    // If CLI path is provided, use it directly
    if (!cliPath.empty()) {
        LOG_VERBOSE("Using CLI-specified test pattern: %s", cliPath.c_str());
        return cliPath;
    }
    
    // List of search locations (in order of priority)
    std::vector<std::string> searchPaths = {
        "testimage.jpg",
        "test_image.jpg",
        "testpattern.png",
        "resources/testimage.jpg",
        "assets/testimage.png"
    };
    
    // Get executable directory
    std::string exeDir = getExecutableDir();
    LOG_VERBOSE("Executable directory: %s", exeDir.c_str());
    
    // Add executable directory and its subdirectories to search paths
    searchPaths.push_back(exeDir + "/testimage.jpg");
    searchPaths.push_back(exeDir + "/test_image.jpg");
    searchPaths.push_back(exeDir + "/testpattern.png");
    searchPaths.push_back(exeDir + "/shaders/testimage.jpg");
    searchPaths.push_back(exeDir + "/shaders/testpattern.png");
    searchPaths.push_back(exeDir + "/assets/testimage.jpg");
    searchPaths.push_back(exeDir + "/assets/testimage.png");
    
    // Search for the first readable file
    for (const auto& path : searchPaths) {
        LOG_VERBOSE("Searching for test pattern at: %s", path.c_str());
        if (fileExists(path)) {
            LOG_VERBOSE("Found test pattern image: %s", path.c_str());
            return path;
        }
    }
    
    LOG_VERBOSE("No test pattern image found in search paths");
    return "";
}

// Load test pattern image using stb_image
static bool loadTestPatternImage(const std::string& imagePath, AppState& state) {
    if (imagePath.empty()) {
        LOG_VERBOSE("No test pattern image path provided");
        return false;
    }
    
    LOG_INFO("Loading test pattern from: %s", imagePath.c_str());
    
    // First, check image dimensions without loading the full image
    int width, height, channels;
    if (!stbi_info(imagePath.c_str(), &width, &height, &channels)) {
        LOG_ERROR("Failed to read image info from '%s': %s", 
                  imagePath.c_str(), stbi_failure_reason());
        return false;
    }
    
    // Validate image dimensions to prevent excessive memory allocation
    if (width > MAX_IMAGE_WIDTH || height > MAX_IMAGE_HEIGHT) {
        LOG_ERROR("Test pattern image '%s' dimensions %dx%d exceed maximum %dx%d",
                  imagePath.c_str(), width, height, MAX_IMAGE_WIDTH, MAX_IMAGE_HEIGHT);
        return false;
    }
    
    if (width <= 0 || height <= 0) {
        LOG_ERROR("Test pattern image '%s' has invalid dimensions %dx%d",
                  imagePath.c_str(), width, height);
        return false;
    }
    
    // Load the image with 4 channels (RGBA)
    unsigned char* data = stbi_load(imagePath.c_str(), &width, &height, &channels, 4);
    
    if (!data) {
        LOG_ERROR("Failed to load test pattern image '%s': %s", 
                  imagePath.c_str(), stbi_failure_reason());
        return false;
    }
    
    LOG_INFO("Loaded test pattern: %dx%d, %d channels", width, height, channels);
    
    // Create and upload texture
    glGenTextures(1, &state.texPattern);
    glBindTexture(GL_TEXTURE_2D, state.texPattern);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    
    stbi_image_free(data);
    
    state.haveTestPattern = true;
    LOG_INFO("Test pattern texture created successfully");
    return true;
}

// Generate procedural test pattern (fallback)
static void generateProceduralPattern(AppState& state) {
    LOG_INFO("Generating procedural test pattern");
    
    const int width = 640;
    const int height = 480;
    std::vector<unsigned char> pattern(width * height * 4);
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            
            // Create color bars pattern
            int bar = (x * 8) / width;
            switch (bar) {
                case 0: pattern[idx] = 255; pattern[idx+1] = 255; pattern[idx+2] = 255; break; // White
                case 1: pattern[idx] = 255; pattern[idx+1] = 255; pattern[idx+2] = 0; break;   // Yellow
                case 2: pattern[idx] = 0;   pattern[idx+1] = 255; pattern[idx+2] = 255; break; // Cyan
                case 3: pattern[idx] = 0;   pattern[idx+1] = 255; pattern[idx+2] = 0; break;   // Green
                case 4: pattern[idx] = 255; pattern[idx+1] = 0;   pattern[idx+2] = 255; break; // Magenta
                case 5: pattern[idx] = 255; pattern[idx+1] = 0;   pattern[idx+2] = 0; break;   // Red
                case 6: pattern[idx] = 0;   pattern[idx+1] = 0;   pattern[idx+2] = 255; break; // Blue
                case 7: pattern[idx] = 0;   pattern[idx+1] = 0;   pattern[idx+2] = 0; break;   // Black
                default: break;
            }
            pattern[idx+3] = 255; // Alpha
        }
    }
    
    glGenTextures(1, &state.texPattern);
    glBindTexture(GL_TEXTURE_2D, state.texPattern);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pattern.data());
    
    state.haveTestPattern = true;
    LOG_INFO("Procedural test pattern generated");
}

// Initialize V4L2 device
static bool initV4L2(const char* device, AppState& state) {
    state.v4l2_fd = open(device, O_RDWR | O_NONBLOCK);
    if (state.v4l2_fd < 0) {
        LOG_ERROR("Failed to open V4L2 device '%s': %s", device, strerror(errno));
        return false;
    }
    
    // Query capabilities
    struct v4l2_capability cap;
    if (ioctl(state.v4l2_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        LOG_ERROR("Failed to query device capabilities: %s", strerror(errno));
        close(state.v4l2_fd);
        return false;
    }
    
    LOG_INFO("V4L2 device: %s", cap.card);
    
    // Set format
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = state.width;
    fmt.fmt.pix.height = state.height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    
    if (ioctl(state.v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
        LOG_ERROR("Failed to set video format: %s", strerror(errno));
        close(state.v4l2_fd);
        return false;
    }
    
    state.width = fmt.fmt.pix.width;
    state.height = fmt.fmt.pix.height;
    LOG_INFO("Video format: %dx%d", state.width, state.height);
    
    // Request buffers
    struct v4l2_requestbuffers reqbuf;
    memset(&reqbuf, 0, sizeof(reqbuf));
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_MMAP;
    reqbuf.count = NUM_BUFFERS;
    
    if (ioctl(state.v4l2_fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
        LOG_ERROR("Failed to request buffers: %s", strerror(errno));
        close(state.v4l2_fd);
        return false;
    }
    
    // Map buffers
    state.buffers.resize(reqbuf.count);
    for (unsigned int i = 0; i < reqbuf.count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        
        if (ioctl(state.v4l2_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            LOG_ERROR("Failed to query buffer %d: %s", i, strerror(errno));
            return false;
        }
        
        state.buffers[i].length = buf.length;
        state.buffers[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                                       MAP_SHARED, state.v4l2_fd, buf.m.offset);
        
        if (state.buffers[i].start == MAP_FAILED) {
            LOG_ERROR("Failed to mmap buffer %d: %s", i, strerror(errno));
            return false;
        }
    }
    
    LOG_INFO("Mapped %zu buffers", state.buffers.size());
    return true;
}

// Queue a buffer with retries
static bool queueBuffer(AppState& state, unsigned int index) {
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    
    for (int retry = 0; retry < QBUF_RETRIES; retry++) {
        if (ioctl(state.v4l2_fd, VIDIOC_QBUF, &buf) == 0) {
            return true;
        }
        LOG_VERBOSE("QBUF retry %d for buffer %u: %s", retry + 1, index, strerror(errno));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    LOG_ERROR("Failed to queue buffer %u after %d retries: %s", 
              index, QBUF_RETRIES, strerror(errno));
    return false;
}

// Start V4L2 streaming
static bool startStreaming(AppState& state) {
    // Queue all buffers
    for (unsigned int i = 0; i < state.buffers.size(); i++) {
        if (!queueBuffer(state, i)) {
            return false;
        }
    }
    
    // Start streaming
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(state.v4l2_fd, VIDIOC_STREAMON, &type) < 0) {
        LOG_ERROR("Failed to start streaming: %s", strerror(errno));
        return false;
    }
    
    state.streaming = true;
    state.lastFrameTime = std::chrono::steady_clock::now();
    LOG_INFO("Streaming started");
    return true;
}

// Stop V4L2 streaming
static void stopStreaming(AppState& state) {
    if (state.streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(state.v4l2_fd, VIDIOC_STREAMOFF, &type);
        state.streaming = false;
        LOG_INFO("Streaming stopped");
    }
}

// Restart V4L2 stream (for error recovery)
static bool restartStream(AppState& state) {
    if (state.stream_reset_count >= STREAM_RESET_RETRIES) {
        LOG_ERROR("Exceeded maximum stream reset attempts (%d)", STREAM_RESET_RETRIES);
        return false;
    }
    
    state.stream_reset_count++;
    LOG_INFO("Restarting stream (attempt %d/%d)", state.stream_reset_count, STREAM_RESET_RETRIES);
    
    stopStreaming(state);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return startStreaming(state);
}

// Dequeue a buffer (returns buffer index or -1 on error)
static int dequeueBuffer(AppState& state, struct v4l2_buffer& buf) {
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    
    if (ioctl(state.v4l2_fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) {
            // No buffer available yet (non-blocking mode)
            return -1;
        }
        
        if (errno == EINVAL) {
            // Stream needs restart
            LOG_VERBOSE("DQBUF returned EINVAL, attempting stream restart");
            if (restartStream(state)) {
                return -1; // Return and try again next frame
            }
            LOG_ERROR("Failed to restart stream after DQBUF EINVAL");
            return -2; // Fatal error
        }
        
        LOG_ERROR("Failed to dequeue buffer: %s", strerror(errno));
        return -2;
    }
    
    // Successful dequeue - reset stream reset count
    state.stream_reset_count = 0;
    state.lastFrameTime = std::chrono::steady_clock::now();
    
    return buf.index;
}

// Check if we should show test pattern (timeout-based fallback)
static void updatePatternState(AppState& state) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - state.lastFrameTime).count();
    
    if (elapsed > PATTERN_TIMEOUT_MS && state.haveTestPattern) {
        if (!state.showPattern) {
            LOG_INFO("No frames received for %dms, showing test pattern", PATTERN_TIMEOUT_MS);
            state.showPattern = true;
        }
    } else {
        if (state.showPattern) {
            LOG_INFO("Frames resumed, hiding test pattern");
            state.showPattern = false;
        }
    }
}

// Print usage information
static void printUsage(const char* progname) {
    fprintf(stderr, "Usage: %s [options]\n", progname);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -d, --device <path>       V4L2 device path (default: /dev/video0)\n");
    fprintf(stderr, "  -t, --test-pattern <path> Path to test pattern image\n");
    fprintf(stderr, "  -v, --verbose             Enable verbose logging\n");
    fprintf(stderr, "  -h, --help                Show this help message\n");
    fprintf(stderr, "\nTest pattern search locations (if --test-pattern not specified):\n");
    fprintf(stderr, "  testimage.jpg, test_image.jpg, testpattern.png\n");
    fprintf(stderr, "  resources/testimage.jpg, assets/testimage.png\n");
    fprintf(stderr, "  <exe_dir>/testimage.jpg, <exe_dir>/shaders/*, <exe_dir>/assets/*\n");
}

// Parse command line arguments
static bool parseArgs(int argc, char* argv[], std::string& device, 
                      std::string& testPattern) {
    device = "/dev/video0";
    testPattern = "";
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "-d" || arg == "--device") {
            if (i + 1 >= argc) {
                LOG_ERROR("Missing argument for %s", arg.c_str());
                return false;
            }
            device = argv[++i];
        } else if (arg == "-t" || arg == "--test-pattern") {
            if (i + 1 >= argc) {
                LOG_ERROR("Missing argument for %s", arg.c_str());
                return false;
            }
            testPattern = argv[++i];
        } else if (arg == "-v" || arg == "--verbose") {
            g_verbose = true;
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            exit(0);
        } else {
            LOG_ERROR("Unknown option: %s", arg.c_str());
            printUsage(argv[0]);
            return false;
        }
    }
    
    return true;
}

// Main function
int main(int argc, char* argv[]) {
    std::string device;
    std::string testPatternPath;
    
    if (!parseArgs(argc, argv, device, testPatternPath)) {
        return 1;
    }
    
    LOG_INFO("HDMI Simple Display starting");
    LOG_VERBOSE("Verbose logging enabled");
    
    AppState state = {};
    state.width = DEFAULT_WIDTH;
    state.height = DEFAULT_HEIGHT;
    state.v4l2_fd = -1;
    state.streaming = false;
    state.stream_reset_count = 0;
    state.haveTestPattern = false;
    state.showPattern = false;
    
    // Initialize V4L2
    if (!initV4L2(device.c_str(), state)) {
        LOG_ERROR("Failed to initialize V4L2");
        // Continue without V4L2 - will show test pattern
    }
    
    // Search for and load test pattern
    // Note: OpenGL/EGL initialization is required before calling loadTestPatternImage
    // and generateProceduralPattern. This demo shows the search and fallback logic.
    std::string foundPattern = findTestPatternImage(testPatternPath);
    if (!foundPattern.empty()) {
#ifdef HAVE_EGL
        if (!loadTestPatternImage(foundPattern, state)) {
            LOG_ERROR("Failed to load test pattern, falling back to procedural pattern");
            generateProceduralPattern(state);
        }
#else
        LOG_INFO("Test pattern image found: %s", foundPattern.c_str());
        LOG_INFO("OpenGL not available - would load test pattern from: %s", foundPattern.c_str());
#endif
    } else {
        LOG_INFO("No test pattern image found, using procedural pattern");
#ifdef HAVE_EGL
        generateProceduralPattern(state);
#endif
    }
    
    // Start streaming if V4L2 is initialized
    if (state.v4l2_fd >= 0) {
        if (!startStreaming(state)) {
            LOG_ERROR("Failed to start streaming");
        }
    }
    
    // Main loop would go here
    LOG_INFO("Main loop would run here (OpenGL rendering)");
    
    // Cleanup
    stopStreaming(state);
    for (auto& buf : state.buffers) {
        if (buf.start != MAP_FAILED) {
            munmap(buf.start, buf.length);
        }
    }
    if (state.v4l2_fd >= 0) {
        close(state.v4l2_fd);
    }
    
    LOG_INFO("HDMI Simple Display exiting");
    return 0;
}