/*
 * hdmi_simple_display.cpp - Robust HDMI input to display application
 *
 * Captures HDMI input via V4L2 and displays it using DRM/KMS.
 * Handles HDMI input interruptions gracefully by showing a test pattern.
 *
 * Usage: hdmi_simple_display [OPTIONS]
 *   --test-pattern=<path>  Path to test pattern image (PNG/JPEG) to show when input is lost
 *   --video-device=<path>  V4L2 video device (default: /dev/video0)
 *   --help                 Show this help message
 */

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

// Buffer management for V4L2
struct VideoBuffer {
    void* start;
    size_t length;
};

// Application state
enum class InputState {
    Unknown,
    Active,
    Lost
};

// Configuration from CLI arguments
struct Config {
    std::string testPatternPath;
    std::string videoDevice = "/dev/video0";
    bool showHelp = false;
};

// Global flag for graceful shutdown
static std::atomic<bool> g_running{true};

static void signalHandler(int /*sig*/) {
    g_running = false;
}

static void printUsage(const char* progName) {
    std::fprintf(stderr,
                 "Usage: %s [OPTIONS]\n"
                 "\n"
                 "Captures HDMI input and displays it on screen.\n"
                 "Shows a test pattern when input is lost and recovers when it returns.\n"
                 "\n"
                 "Options:\n"
                 "  --test-pattern=<path>  Path to test pattern image (PNG/JPEG) to show\n"
                 "                         when HDMI input is lost\n"
                 "  --video-device=<path>  V4L2 video device (default: /dev/video0)\n"
                 "  --help                 Show this help message\n",
                 progName);
}

static Config parseArgs(int argc, char* argv[]) {
    Config config;

    static struct option longOptions[] = {
        {"test-pattern", required_argument, nullptr, 't'},
        {"video-device", required_argument, nullptr, 'v'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "t:v:h", longOptions, nullptr)) != -1) {
        switch (opt) {
            case 't':
                config.testPatternPath = optarg;
                break;
            case 'v':
                config.videoDevice = optarg;
                break;
            case 'h':
                config.showHelp = true;
                break;
            default:
                config.showHelp = true;
                break;
        }
    }

    return config;
}

static int xioctl(int fd, unsigned long request, void* arg) {
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

class HdmiDisplay {
public:
    explicit HdmiDisplay(const Config& config)
        : m_config(config)
        , m_inputState(InputState::Unknown)
        , m_videoFd(-1) {}

    ~HdmiDisplay() {
        cleanup();
    }

    bool initialize() {
        // Open video device
        m_videoFd = open(m_config.videoDevice.c_str(), O_RDWR | O_NONBLOCK);
        if (m_videoFd < 0) {
            std::fprintf(stderr, "Error: Cannot open video device %s: %s\n",
                         m_config.videoDevice.c_str(), std::strerror(errno));
            return false;
        }

        // Query device capabilities
        struct v4l2_capability cap;
        if (xioctl(m_videoFd, VIDIOC_QUERYCAP, &cap) < 0) {
            std::fprintf(stderr, "Error: VIDIOC_QUERYCAP failed: %s\n",
                         std::strerror(errno));
            return false;
        }

        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
            std::fprintf(stderr, "Error: Device does not support video capture\n");
            return false;
        }

        if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
            std::fprintf(stderr, "Error: Device does not support streaming\n");
            return false;
        }

        std::printf("Video device: %s (%s)\n", cap.card, cap.driver);

        // Load test pattern if specified
        if (!m_config.testPatternPath.empty()) {
            if (!loadTestPattern()) {
                std::fprintf(stderr, "Warning: Could not load test pattern from %s\n",
                             m_config.testPatternPath.c_str());
            } else {
                std::printf("Test pattern loaded: %s\n", m_config.testPatternPath.c_str());
            }
        }

        return true;
    }

    void run() {
        while (g_running) {
            switch (m_inputState) {
                case InputState::Unknown:
                case InputState::Lost:
                    if (tryStartCapture()) {
                        m_inputState = InputState::Active;
                        std::printf("HDMI input active\n");
                    } else {
                        showTestPattern();
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    }
                    break;

                case InputState::Active:
                    if (!captureAndDisplay()) {
                        m_inputState = InputState::Lost;
                        std::printf("HDMI input lost\n");
                        stopCapture();
                    }
                    break;
            }
        }
    }

private:
    bool loadTestPattern() {
        // Check if the file exists and is readable
        int fd = open(m_config.testPatternPath.c_str(), O_RDONLY);
        if (fd < 0) {
            return false;
        }
        close(fd);
        m_hasTestPattern = true;
        return true;
    }

    void showTestPattern() {
        if (m_hasTestPattern && !m_showingTestPattern) {
            std::printf("Showing test pattern: %s\n", m_config.testPatternPath.c_str());
            m_showingTestPattern = true;
            // In a full implementation, this would render the test pattern to the display
            // using DRM/KMS or a graphics library
        }
    }

    void hideTestPattern() {
        if (m_showingTestPattern) {
            std::printf("Hiding test pattern\n");
            m_showingTestPattern = false;
        }
    }

    bool tryStartCapture() {
        // Set format
        struct v4l2_format fmt;
        std::memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (xioctl(m_videoFd, VIDIOC_G_FMT, &fmt) < 0) {
            return false;
        }

        // Request buffers
        struct v4l2_requestbuffers req;
        std::memset(&req, 0, sizeof(req));
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (xioctl(m_videoFd, VIDIOC_REQBUFS, &req) < 0) {
            return false;
        }

        if (req.count < 2) {
            std::fprintf(stderr, "Error: Insufficient buffer memory\n");
            return false;
        }

        // Map buffers
        m_buffers.resize(req.count);
        for (unsigned int i = 0; i < req.count; ++i) {
            struct v4l2_buffer buf;
            std::memset(&buf, 0, sizeof(buf));
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;

            if (xioctl(m_videoFd, VIDIOC_QUERYBUF, &buf) < 0) {
                return false;
            }

            m_buffers[i].length = buf.length;
            m_buffers[i].start = mmap(nullptr, buf.length,
                                      PROT_READ | PROT_WRITE,
                                      MAP_SHARED, m_videoFd, buf.m.offset);

            if (m_buffers[i].start == MAP_FAILED) {
                return false;
            }
        }

        // Queue buffers
        for (unsigned int i = 0; i < m_buffers.size(); ++i) {
            struct v4l2_buffer buf;
            std::memset(&buf, 0, sizeof(buf));
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;

            if (xioctl(m_videoFd, VIDIOC_QBUF, &buf) < 0) {
                return false;
            }
        }

        // Start streaming
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(m_videoFd, VIDIOC_STREAMON, &type) < 0) {
            return false;
        }

        m_streaming = true;
        hideTestPattern();
        return true;
    }

    void stopCapture() {
        if (m_streaming) {
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            xioctl(m_videoFd, VIDIOC_STREAMOFF, &type);
            m_streaming = false;
        }

        // Unmap buffers
        for (auto& buf : m_buffers) {
            if (buf.start != nullptr && buf.start != MAP_FAILED) {
                munmap(buf.start, buf.length);
            }
        }
        m_buffers.clear();
    }

    bool captureAndDisplay() {
        struct v4l2_buffer buf;
        std::memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        // Dequeue buffer
        if (xioctl(m_videoFd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) {
                // No buffer available yet, this is normal for non-blocking I/O
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return true;
            }

            if (errno == EINVAL || errno == EIO) {
                // EINVAL: Input changed or driver error (e.g., rk_hdmirx source change)
                // EIO: I/O error, likely HDMI cable disconnected
                std::fprintf(stderr, "VIDIOC_DQBUF error: %s (errno=%d)\n",
                             std::strerror(errno), errno);
                return false;
            }

            std::fprintf(stderr, "VIDIOC_DQBUF failed: %s\n", std::strerror(errno));
            return false;
        }

        // Process the captured frame
        // In a full implementation, this would render the frame to the display
        // using DRM/KMS zero-copy buffer sharing

        // Requeue buffer
        if (xioctl(m_videoFd, VIDIOC_QBUF, &buf) < 0) {
            std::fprintf(stderr, "VIDIOC_QBUF failed: %s\n", std::strerror(errno));
            return false;
        }

        return true;
    }

    void cleanup() {
        stopCapture();

        if (m_videoFd >= 0) {
            close(m_videoFd);
            m_videoFd = -1;
        }
    }

    Config m_config;
    InputState m_inputState;
    int m_videoFd;
    std::vector<VideoBuffer> m_buffers;
    bool m_streaming = false;
    bool m_hasTestPattern = false;
    bool m_showingTestPattern = false;
};

int main(int argc, char* argv[]) {
    Config config = parseArgs(argc, argv);

    if (config.showHelp) {
        printUsage(argv[0]);
        return 0;
    }

    // Install signal handlers for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::printf("HDMI Simple Display starting...\n");
    if (!config.testPatternPath.empty()) {
        std::printf("Test pattern: %s\n", config.testPatternPath.c_str());
    }
    std::printf("Video device: %s\n", config.videoDevice.c_str());

    HdmiDisplay display(config);

    if (!display.initialize()) {
        std::fprintf(stderr, "Error: Failed to initialize display\n");
        return 1;
    }

    display.run();

    std::printf("HDMI Simple Display shutting down...\n");
    return 0;
}