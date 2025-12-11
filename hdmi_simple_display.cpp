// hdmi_simple_display.cpp
// HDMI YUV display with shader test pattern and improved safe automatic recovery.
// Background thread does V4L2 only and verifies actual frames before signalling GL thread.
//
// Build:
//   rm -rf build && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <getopt.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <algorithm>
#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <sys/stat.h>
#include <limits.h>
#include <sstream>
#include <array>
#include <cctype>
#include <thread>
#include <memory>
#include <atomic>
#include <cmath>
#include <mutex>
#include <chrono>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define DEVICE "/dev/video0"
#define DEFAULT_WIDTH 1920
#define DEFAULT_HEIGHT 1080
#define WINDOW_TITLE "hdmi_simple_display (OpenGL YUV Shader)"
#define BUF_COUNT 4 // MMAP buffer count

static bool opt_auto_resize_window = false;
static bool opt_cpu_uv_swap = false;
static int opt_uv_swap_override = -1;
static int opt_full_range = 0;
static int opt_use_bt709 = 1;
static bool opt_verbose = false;
static std::string opt_test_pattern_path;

static const bool ENABLE_SHADER_TEST_PATTERN = true;
static const int PATTERN_TIMEOUT_MS = 800;
static const int RECOVERY_GRACE_MS = 3000;
static const int QBUF_RETRIES = 5;
static const int QBUF_RETRY_MS = 10;
static const int EINVAL_RESTART_THRESHOLD = 8;
static const int STREAM_RESTART_RETRIES = 3;
static const int STREAM_RESTART_BACKOFF_MS = 150;

// Verification/retry parameters for background reopen
static const int BG_REOPEN_VERIFY_MAX_ATTEMPTS = 6;
static const int BG_REOPEN_VERIFY_POLL_MS = 200; // poll fd to check frames

static inline void vlog(const std::string &s) { if (opt_verbose) std::cerr << s; }
static inline void vlogln(const std::string &s) { if (opt_verbose) std::cerr << s << std::endl; }

std::string loadShaderSource(const char* filename) {
    std::ifstream file(filename);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

int xioctl(int fd, int req, void* arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

GLuint compileShader(const std::string& source, GLenum type) {
    GLuint shader = glCreateShader(type);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint status; glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        std::string msg; msg.resize(16384);
        glGetShaderInfoLog(shader, (GLsizei)msg.size(), nullptr, &msg[0]);
        std::cerr << "Shader compilation failed: " << msg << std::endl;
        exit(EXIT_FAILURE);
    }
    return shader;
}

GLuint createShaderProgram(const char* vert_path, const char* frag_path) {
    auto vert_source = loadShaderSource(vert_path);
    auto frag_source = loadShaderSource(frag_path);
    GLuint vert = compileShader(vert_source, GL_VERTEX_SHADER);
    GLuint frag = compileShader(frag_source, GL_FRAGMENT_SHADER);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);
    GLint status; glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (!status) {
        std::string msg; msg.resize(16384);
        glGetProgramInfoLog(prog, (GLsizei)msg.size(), nullptr, &msg[0]);
        std::cerr << "Shader link failed: " << msg << std::endl;
        exit(EXIT_FAILURE);
    }
    glDeleteShader(vert); glDeleteShader(frag);
    return prog;
}

struct PlaneMap { void* addr; size_t length; };

std::string fourcc_to_str(uint32_t f) {
    char s[5] = { (char)(f & 0xFF), (char)((f>>8)&0xFF), (char)((f>>16)&0xFF), (char)((f>>24)&0xFF), 0 };
    return std::string(s);
}

bool get_v4l2_format(int fd, uint32_t &width, uint32_t &height, uint32_t &pixelformat) {
    v4l2_format fmt; memset(&fmt,0,sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) return false;
    width = fmt.fmt.pix_mp.width; height = fmt.fmt.pix_mp.height; pixelformat = fmt.fmt.pix_mp.pixelformat;
    return true;
}

void reallocate_textures(GLuint texY, GLuint texUV, int newW, int newH, int uvW, int uvH) {
    glBindTexture(GL_TEXTURE_2D, texY);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, newW, newH, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, texUV);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, uvW, uvH, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
}

void upload_texture_tiled(GLenum format, GLuint tex, int srcW, int srcH,
                          const unsigned char* src, int maxTexSize, int pixelSizePerTexel) {
    int tileW = std::min(srcW, maxTexSize), tileH = std::min(srcH, maxTexSize);
    static std::vector<unsigned char> tileBuf;
    for (int y = 0; y < srcH; y += tileH) {
        int h = std::min(tileH, srcH - y);
        if (srcW <= maxTexSize) {
            const unsigned char* ptr = src + (size_t)y * (size_t)srcW * pixelSizePerTexel;
            glBindTexture(GL_TEXTURE_2D, tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, srcW, h, format, GL_UNSIGNED_BYTE, ptr);
        } else {
            for (int x = 0; x < srcW; x += tileW) {
                int w = std::min(tileW, srcW - x);
                tileBuf.resize((size_t)h * (size_t)w * pixelSizePerTexel);
                for (int row = 0; row < h; ++row) {
                    const unsigned char* srcRow = src + (size_t)(y + row) * (size_t)srcW * pixelSizePerTexel + (size_t)x * pixelSizePerTexel;
                    unsigned char* dstRow = tileBuf.data() + (size_t)row * (size_t)w * pixelSizePerTexel;
                    memcpy(dstRow, srcRow, (size_t)w * pixelSizePerTexel);
                }
                glBindTexture(GL_TEXTURE_2D, tex);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, format, GL_UNSIGNED_BYTE, tileBuf.data());
            }
        }
    }
}

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "  --uv-swap=auto|0|1\n"
              << "  --range=limited|full\n"
              << "  --matrix=709|601\n"
              << "  --auto-resize-window\n"
              << "  --cpu-uv-swap\n"
              << "  --test-pattern=<path>\n"
              << "  --verbose\n"
              << "  -h, --help\n";
}

static std::string getExecutableDir() {
    char* base = SDL_GetBasePath();
    if (base) {
        std::string dir(base); SDL_free(base);
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir.push_back('/');
        return dir;
    }
#if defined(__linux__)
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf)-1);
    if (len>0) { buf[len]=0; std::string p(buf); auto pos = p.find_last_of('/'); if (pos!=std::string::npos) return p.substr(0,pos+1); }
#endif
    return std::string("./");
}

static bool fileExists(const std::string &path) {
    struct stat sb; return (stat(path.c_str(), &sb) == 0 && S_ISREG(sb.st_mode));
}

static std::string findShaderFile(const std::string &name, std::vector<std::string>* outAttempts = nullptr) {
    if (name.empty()) return std::string();
    std::vector<std::string> candidates; candidates.push_back(name);
    std::string exeDir = getExecutableDir();
    if (!exeDir.empty()) {
        candidates.push_back(exeDir + name); candidates.push_back(exeDir + "shaders/" + name);
        candidates.push_back(exeDir + "../" + name); candidates.push_back(exeDir + "../shaders/" + name);
        candidates.push_back(exeDir + "../../shaders/" + name); candidates.push_back(exeDir + "assets/" + name);
    }
    candidates.push_back(std::string("shaders/") + name);
    candidates.push_back(std::string("/usr/local/share/hdmi-in-display/shaders/") + name);
    candidates.push_back(std::string("/usr/share/hdmi-in-display/shaders/") + name);
    if (outAttempts) { outAttempts->clear(); outAttempts->reserve(candidates.size()); }
    for (const auto &p : candidates) { if (outAttempts) outAttempts->push_back(p); if (fileExists(p)) return p; }
    return std::string();
}

// ControlParams (unchanged)
struct ControlParams {
    float fullInputW = 3840.0f; float fullInputH = 2160.0f;
    int segmentsX = 3; int segmentsY = 3;
    float subBlockW = 1280.0f; float subBlockH = 720.0f;
    float tileW = 128.0f; float tileH = 144.0f;
    float spacingX = 98.0f; float spacingY = 90.0f;
    float marginX = 0.0f; int numTilesPerRow = 10; int numTilesPerCol = 15;
    int inputTilesTopToBottom = 1;
    int moduleSerials[3] = {0,0,0};
};

static std::array<std::string,3> buildModuleFilenames(const ControlParams &ctrl) {
    std::array<std::string,3> names;
    for (int i=0;i<3;++i) names[i] = (ctrl.moduleSerials[i]==0) ? std::string("modul") + std::to_string(i+1) + ".txt" : std::string("m") + std::to_string(ctrl.moduleSerials[i]) + ".txt";
    return names;
}

static bool loadControlIni(const std::string &path, ControlParams &out) {
    std::ifstream f; std::string exeDir = getExecutableDir(); std::string candidates[2] = { exeDir + path, path };
    for (int c=0;c<2;++c) {
        if (!fileExists(candidates[c])) continue;
        f.open(candidates[c]); if (!f.is_open()) continue;
        std::string line;
        while (std::getline(f, line)) {
            size_t s = line.find_first_not_of(" \t\r\n"); if (s==std::string::npos) continue;
            if (line[s]=='#') continue;
            size_t eq = line.find('='); if (eq==std::string::npos) continue;
            std::string key = line.substr(0,eq); std::string val = line.substr(eq+1);
            auto trim = [](std::string &str){ size_t a = str.find_first_not_of(" \t\r\n"); if (a==std::string::npos) { str.clear(); return; } size_t b = str.find_last_not_of(" \t\r\n"); str = str.substr(a,b-a+1); };
            trim(key); trim(val); if (key.empty()||val.empty()) continue;
            if (key=="fullInputSize") { int a=0,b=0; if (sscanf(val.c_str(), "%d,%d", &a,&b)==2) { out.fullInputW=(float)a; out.fullInputH=(float)b; } }
            else if (key=="segments") { int a=0,b=0; if (sscanf(val.c_str(), "%d,%d",&a,&b)==2) { out.segmentsX=a; out.segmentsY=b; } }
            else if (key=="subBlockSize") { int a=0,b=0; if (sscanf(val.c_str(), "%d,%d",&a,&b)==2) { out.subBlockW=(float)a; out.subBlockH=(float)b; } }
            else if (key=="tileSize") { int a=0,b=0; if (sscanf(val.c_str(), "%d,%d",&a,&b)==2) { out.tileW=(float)a; out.tileH=(float)b; } }
            else if (key=="spacing") { int a=0,b=0; if (sscanf(val.c_str(), "%d,%d",&a,&b)==2) { out.spacingX=(float)a; out.spacingY=(float)b; } }
            else if (key=="marginX") out.marginX = (float)atoi(val.c_str());
            else if (key=="numTiles") { int a=0,b=0; if (sscanf(val.c_str(), "%d,%d",&a,&b)==2) { out.numTilesPerRow=a; out.numTilesPerCol=b; } }
            else if (key=="inputTilesTopToBottom") out.inputTilesTopToBottom = atoi(val.c_str())?1:0;
            else if (key=="moduleSerials") { int a=0,b=0,c2=0; if (sscanf(val.c_str(), "%d,%d,%d",&a,&b,&c2)>=1) { out.moduleSerials[0]=a; out.moduleSerials[1]=b; out.moduleSerials[2]=c2; } }
            else if (key=="modul1Serial") out.moduleSerials[0] = atoi(val.c_str());
            else if (key=="modul2Serial") out.moduleSerials[1] = atoi(val.c_str());
            else if (key=="modul3Serial") out.moduleSerials[2] = atoi(val.c_str());
            else if (key=="verbose") opt_verbose = atoi(val.c_str()) != 0;
            else if (key=="testPattern") opt_test_pattern_path = val;
        }
        f.close(); return true;
    }
    return false;
}

// async save frame unchanged
static void async_save_frame_to_png(std::vector<unsigned char> ybuf,
                                    std::vector<unsigned char> uvbuf,
                                    std::vector<unsigned char> packed,
                                    size_t packedSize,
                                    int width, int height,
                                    uint32_t pixfmt,
                                    int uv_swap_flag, int use_bt709_flag, int full_range_flag,
                                    std::string filename_png)
{
    std::string fourcc = fourcc_to_str(pixfmt);
    const int comp = 3;
    std::vector<unsigned char> rgb;
    try { rgb.resize((size_t)width * (size_t)height * comp); } catch (...) { return; }
    size_t ylen = (size_t)width * (size_t)height;
    size_t uvlen = (size_t)width * ((size_t)height / 2);
    if (pixfmt == V4L2_PIX_FMT_NV12 || pixfmt == V4L2_PIX_FMT_NV21 ||
        (ybuf.empty() && uvbuf.empty() && packedSize == (ylen + uvlen))) {
        if (ybuf.empty() && uvbuf.empty()) {
            ybuf.assign(packed.begin(), packed.begin() + ylen);
            uvbuf.assign(packed.begin() + ylen, packed.begin() + ylen + uvlen);
        }
        for (int yy = 0; yy < height; ++yy) {
            int uv_row = yy / 2;
            for (int xx = 0; xx < width; ++xx) {
                int yi = yy * width + xx;
                int uv_col = (xx / 2);
                size_t uvIndex = (size_t)uv_row * (size_t)width + (size_t)uv_col * 2;
                unsigned char Yc = (yi < (int)ybuf.size()) ? ybuf[yi] : 0;
                unsigned char Uc = (uvIndex + 1 < uvbuf.size() && uvbuf.size()>0) ? uvbuf[uvIndex + (uv_swap_flag ? 1 : 0)] : 128;
                unsigned char Vc = (uvIndex + 1 < uvbuf.size() && uvbuf.size()>0) ? uvbuf[uvIndex + (uv_swap_flag ? 0 : 1)] : 128;
                float Yf = (float)Yc;
                float Uf = (float)Uc - 128.0f;
                float Vf = (float)Vc - 128.0f;
                float rf, gf, bf;
                if (full_range_flag == 1) {
                    if (use_bt709_flag == 1) {
                        rf = Yf + 1.792741f * Vf;
                        gf = Yf - 0.213249f * Uf - 0.532909f * Vf;
                        bf = Yf + 2.112402f * Uf;
                    } else {
                        rf = Yf + 1.596027f * Vf;
                        gf = Yf - 0.391762f * Uf - 0.812968f * Vf;
                        bf = Yf + 2.017232f * Uf;
                    }
                } else {
                    float y_lin = 1.164383f * (Yf - 16.0f);
                    if (use_bt709_flag == 1) {
                        rf = y_lin + 1.792741f * Vf;
                        gf = y_lin - 0.213249f * Uf - 0.532909f * Vf;
                        bf = y_lin + 2.112402f * Uf;
                    } else {
                        rf = y_lin + 1.596027f * Vf;
                        gf = y_lin - 0.391762f * Uf - 0.812968f * Vf;
                        bf = y_lin + 2.017232f * Uf;
                    }
                }
                int r = (int)std::round(std::max(0.0f, std::min(255.0f, rf)));
                int g = (int)std::round(std::max(0.0f, std::min(255.0f, gf)));
                int b = (int)std::round(std::max(0.0f, std::min(255.0f, bf)));
                size_t outIdx = ((size_t)yy * (size_t)width + (size_t)xx) * comp;
                rgb[outIdx + 0] = (unsigned char)r;
                rgb[outIdx + 1] = (unsigned char)g;
                rgb[outIdx + 2] = (unsigned char)b;
            }
        }
    } else {
        for (size_t i=0;i<rgb.size();++i) rgb[i]=0;
    }
    int stride = width * comp;
    stbi_write_png(filename_png.c_str(), width, height, comp, rgb.data(), stride);
}

// restart_v4l_stream unchanged (V4L2-only)
static bool restart_v4l_stream(int &fd, std::vector<std::vector<PlaneMap>> &buffers) {
    if (fd >= 0) {
        int typeoff = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (ioctl(fd, VIDIOC_STREAMOFF, &typeoff) < 0) {
            if (opt_verbose) vlogln(std::string("restart_v4l_stream: STREAMOFF failed: ") + strerror(errno));
        }
        for (auto &bvec : buffers) {
            for (auto &pm : bvec) {
                if (pm.addr && pm.length) { munmap(pm.addr, pm.length); pm.addr=nullptr; pm.length=0; }
            }
        }
        buffers.clear();
    }

    v4l2_requestbuffers req; memset(&req,0,sizeof(req));
    req.count = BUF_COUNT; req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        if (opt_verbose) vlogln(std::string("restart_v4l_stream: VIDIOC_REQBUFS failed: ") + strerror(errno));
        return false;
    }
    if (req.count == 0) { if (opt_verbose) vlogln("restart_v4l_stream: VIDIOC_REQBUFS returned zero buffers"); return false; }

    buffers.resize(req.count);
    for (unsigned i=0;i<req.count;++i) {
        v4l2_buffer bufq; v4l2_plane planes[VIDEO_MAX_PLANES];
        memset(&bufq,0,sizeof(bufq)); memset(planes,0,sizeof(planes));
        bufq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; bufq.index = i; bufq.memory = V4L2_MEMORY_MMAP; bufq.m.planes = planes; bufq.length = VIDEO_MAX_PLANES;
        if (xioctl(fd, VIDIOC_QUERYBUF, &bufq) < 0) {
            if (opt_verbose) vlogln(std::string("restart_v4l_stream: VIDIOC_QUERYBUF failed: ") + strerror(errno));
            for (auto &bvec : buffers) for (auto &pm : bvec) if (pm.addr && pm.length) munmap(pm.addr, pm.length);
            buffers.clear(); return false;
        }
        buffers[i].resize(bufq.length);
        for (unsigned p=0;p<bufq.length;++p) {
            buffers[i][p].length = planes[p].length;
            buffers[i][p].addr = mmap(nullptr, planes[p].length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, planes[p].m.mem_offset);
            if (buffers[i][p].addr == MAP_FAILED) {
                if (opt_verbose) vlogln(std::string("restart_v4l_stream: mmap failed: ") + strerror(errno));
                for (auto &bvec : buffers) for (auto &pm : bvec) if (pm.addr && pm.length) munmap(pm.addr, pm.length);
                buffers.clear(); return false;
            }
        }
        if (xioctl(fd, VIDIOC_QBUF, &bufq) < 0) {
            if (opt_verbose) vlogln(std::string("restart_v4l_stream: VIDIOC_QBUF failed: ") + strerror(errno));
            for (auto &bvec : buffers) for (auto &pm : bvec) if (pm.addr && pm.length) munmap(pm.addr, pm.length);
            buffers.clear(); return false;
        }
    }

    int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) {
        if (opt_verbose) vlogln(std::string("restart_v4l_stream: VIDIOC_STREAMON failed: ") + strerror(errno));
        for (auto &bvec : buffers) for (auto &pm : bvec) if (pm.addr && pm.length) munmap(pm.addr, pm.length);
        buffers.clear(); return false;
    }
    if (opt_verbose) vlogln("restart_v4l_stream: stream restart successful");
    return true;
}

// Forward declarations so loadOffsetsFromModuleFiles can appear earlier if needed:
static std::string joinPath(const std::string &dir, const std::string &name);
static bool parseXYLine(const std::string &line, int &x, int &y);

// Implementation of loadOffsetsFromModuleFiles
static bool loadOffsetsFromModuleFiles(const std::array<std::string,3> &names, std::vector<GLint> &out) {
    out.assign(150 * 2, 0);
    size_t fillIndex = 0;
    std::string exeDir = getExecutableDir();
    for (size_t m = 0; m < names.size(); ++m) {
        std::string candidates[2] = { joinPath(exeDir, names[m]), names[m] };
        for (int c = 0; c < 2; ++c) {
            const std::string &path = candidates[c];
            if (!fileExists(path)) continue;
            std::ifstream f(path);
            if (!f.is_open()) continue;
            std::string line;
            while (std::getline(f, line) && fillIndex < out.size()) {
                int x = 0, y = 0;
                if (!parseXYLine(line, x, y)) continue;
                out[fillIndex++] = (GLint)x;
                out[fillIndex++] = (GLint)y;
            }
            break; // stop after first found candidate for this module
        }
    }
    return true;
}

// Provide definitions for the small helpers (joinPath/parseXYLine) so the linker is happy:
static std::string joinPath(const std::string &dir, const std::string &name) {
    if (dir.empty()) return name;
    if (dir.back() == '/' || dir.back() == '\\') return dir + name;
    return dir + "/" + name;
}

static bool parseXYLine(const std::string &line, int &x, int &y) {
    size_t a = line.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return false;
    size_t b = line.find_last_not_of(" \t\r\n");
    std::string s = line.substr(a, b - a + 1);
    if (s.empty()) return false;
    if (s[0] == '#') return false;
    std::istringstream iss(s);
    if (!(iss >> x >> y)) return false;
    return true;
}

int main(int argc, char** argv) {
    vlogln("startup: begin");

    static struct option longopts[] = {
      {"uv-swap", required_argument, nullptr, 0},
      {"range", required_argument, nullptr, 0},
      {"matrix", required_argument, nullptr, 0},
      {"auto-resize-window", no_argument, nullptr, 0},
      {"cpu-uv-swap", no_argument, nullptr, 0},
      {"test-pattern", required_argument, nullptr, 0},
      {"verbose", no_argument, nullptr, 0},
      {"help", no_argument, nullptr, 'h'},
      {0,0,0,0}
    };

    for (;;) {
      int idx = 0;
      int c = getopt_long(argc, argv, "h", longopts, &idx);
      if (c == -1) break;
      if (c == 'h') { print_usage(argv[0]); return 0; }
      if (c == 0) {
        std::string name = longopts[idx].name;
        if (name == "uv-swap") { std::string v = optarg ? optarg : "auto"; if (v=="auto") opt_uv_swap_override = -1; else if (v=="0") opt_uv_swap_override=0; else if (v=="1") opt_uv_swap_override=1; else { std::cerr<<"Invalid uv-swap\n"; print_usage(argv[0]); return 1; } }
        else if (name == "range") { std::string v = optarg ? optarg : "limited"; if (v=="limited") opt_full_range=0; else if (v=="full") opt_full_range=1; else { std::cerr<<"Invalid range\n"; print_usage(argv[0]); return 1; } }
        else if (name == "matrix") { std::string v = optarg ? optarg : "709"; if (v=="709") opt_use_bt709=1; else if (v=="601") opt_use_bt709=0; else { std::cerr<<"Invalid matrix\n"; print_usage(argv[0]); return 1; } }
        else if (name == "auto-resize-window") opt_auto_resize_window = true;
        else if (name == "cpu-uv-swap") opt_cpu_uv_swap = true;
        else if (name == "test-pattern") if (optarg) opt_test_pattern_path = std::string(optarg);
        else if (name == "verbose") opt_verbose = true;
      }
    }

    int fd = open(DEVICE, O_RDWR | O_NONBLOCK);
    if (fd < 0) { perror("open video0"); return 1; }

    uint32_t cur_width = DEFAULT_WIDTH, cur_height = DEFAULT_HEIGHT, cur_pixfmt = 0;
    if (!get_v4l2_format(fd, cur_width, cur_height, cur_pixfmt)) { cur_width = DEFAULT_WIDTH; cur_height = DEFAULT_HEIGHT; }

    v4l2_format fmt; memset(&fmt,0,sizeof(fmt)); fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = cur_width; fmt.fmt.pix_mp.height = cur_height; fmt.fmt.pix_mp.pixelformat = v4l2_fourcc('N','V','2','4');
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE; fmt.fmt.pix_mp.num_planes = 1;
    (void)xioctl(fd, VIDIOC_S_FMT, &fmt);
    get_v4l2_format(fd, cur_width, cur_height, cur_pixfmt);

    v4l2_event_subscription sub; memset(&sub,0,sizeof(sub)); sub.type = V4L2_EVENT_SOURCE_CHANGE;
    if (ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) { /* not fatal */ }

    v4l2_requestbuffers req; memset(&req,0,sizeof(req));
    req.count = BUF_COUNT; req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("VIDIOC_REQBUFS"); close(fd); return 1; }

    std::vector<std::vector<PlaneMap>> buffers(req.count);
    for (unsigned i=0;i<req.count;++i) {
        v4l2_buffer buf; v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
        memset(&buf,0,sizeof(buf)); buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; buf.index = i; buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes; buf.length = VIDEO_MAX_PLANES;
        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) { perror("VIDIOC_QUERYBUF"); close(fd); return 1; }
        buffers[i].resize(buf.length);
        for (unsigned p=0;p<buf.length;++p) {
            buffers[i][p].length = planes[p].length;
            buffers[i][p].addr = mmap(nullptr, planes[p].length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, planes[p].m.mem_offset);
            if (buffers[i][p].addr == MAP_FAILED) { perror("mmap plane"); close(fd); return 1; }
        }
        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("VIDIOC_QBUF"); close(fd); return 1; }
    }

    int buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &buf_type) < 0) { perror("VIDIOC_STREAMON"); close(fd); return 1; }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl; close(fd); return 1; }
    vlogln("startup: SDL initialized");

    SDL_Window* win = SDL_CreateWindow(WINDOW_TITLE, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, (int)cur_width, (int)cur_height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!win) { std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl; close(fd); return 1; }
    vlogln("startup: SDL window created");

    if (SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) { std::cerr << "Warning: could not set fullscreen: " << SDL_GetError() << "\n"; }
    else vlogln("Window set to FULLSCREEN_DESKTOP on startup");

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_GLContext glc = SDL_GL_CreateContext(win);
    if (!glc) { std::cerr << "SDL_GL_CreateContext failed: " << SDL_GetError() << std::endl; close(fd); return 1; }
    vlogln("startup: GL context created");

    if (glewInit() != GLEW_OK) { std::cerr << "GLEW init failed!" << std::endl; close(fd); return 1; }

    GLint gl_max_tex=0; glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gl_max_tex);
    int win_w = 0, win_h = 0;
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0) {
        win_w = dm.w;
        win_h = dm.h;
        SDL_SetWindowSize(win, win_w, win_h);
    } else {
        SDL_GetWindowSize(win, &win_w, &win_h);
    }
    glViewport(0, 0, win_w, win_h);

    std::vector<std::string> attempts; std::string vertPath = findShaderFile("shader.vert.glsl",&attempts);
    if (vertPath.empty()) { std::cerr<<"Vertex shader not found\n"; close(fd); return 1; }
    attempts.clear(); std::string fragPath = findShaderFile("shader.frag.glsl",&attempts);
    if (fragPath.empty()) { std::cerr<<"Fragment shader not found\n"; close(fd); return 1; }

    GLuint program = createShaderProgram(vertPath.c_str(), fragPath.c_str()); glUseProgram(program);
    vlogln("startup: shader program created");

    float verts[] = { -1,-1,0,0, 1,-1,1,0, -1,1,0,1, 1,1,1,1 };
    GLuint vbo=0, vao=0; glGenBuffers(1,&vbo); glGenVertexArrays(1,&vao);
    glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)(2*sizeof(float)));
    glBindBuffer(GL_ARRAY_BUFFER,0); glBindVertexArray(0);

    GLuint texY=0, texUV=0, texPattern=0; glGenTextures(1,&texY); glBindTexture(GL_TEXTURE_2D,texY);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glGenTextures(1,&texUV); glBindTexture(GL_TEXTURE_2D,texUV);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);

    int uv_w = (cur_pixfmt == V4L2_PIX_FMT_NV12 || cur_pixfmt == V4L2_PIX_FMT_NV21) ? (int)(cur_width/2) : (int)cur_width;
    int uv_h = (cur_pixfmt == V4L2_PIX_FMT_NV12 || cur_pixfmt == V4L2_PIX_FMT_NV21) ? (int)(cur_height/2) : (int)cur_height;
    reallocate_textures(texY, texUV, (int)cur_width, (int)cur_height, uv_w, uv_h);

    bool haveTestPattern = false; int patternW=0,patternH=0,patternComp=0;
    if (!opt_test_pattern_path.empty() && fileExists(opt_test_pattern_path)) {
        unsigned char *img = stbi_load(opt_test_pattern_path.c_str(), &patternW, &patternH, &patternComp, 3);
        if (img) {
            glGenTextures(1,&texPattern); glBindTexture(GL_TEXTURE_2D, texPattern);
            glPixelStorei(GL_UNPACK_ALIGNMENT,1); glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,patternW,patternH,0,GL_RGB,GL_UNSIGNED_BYTE,img);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
            stbi_image_free(img); haveTestPattern=true; vlogln("Loaded test pattern image into GL texture");
        } else std::cerr<<"Failed to load test pattern image: "<<opt_test_pattern_path<<"\n";
    } else {
        if (opt_test_pattern_path.empty()) {
            std::vector<std::string> cands = { "testimage.jpg","test_image.jpg","testpattern.png","testpattern.jpg","resources/testimage.jpg","assets/testimage.jpg" };
            std::string exe = getExecutableDir();
            if (!exe.empty()) { cands.push_back(exe + "testimage.jpg"); cands.push_back(exe + "shaders/testimage.jpg"); }
            for (auto &p : cands) if (!p.empty() && fileExists(p)) { opt_test_pattern_path = p; break; }
            if (!opt_test_pattern_path.empty()) {
                unsigned char *img = stbi_load(opt_test_pattern_path.c_str(), &patternW, &patternH, &patternComp, 3);
                if (img) {
                    glGenTextures(1,&texPattern); glBindTexture(GL_TEXTURE_2D, texPattern);
                    glPixelStorei(GL_UNPACK_ALIGNMENT,1); glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,patternW,patternH,0,GL_RGB,GL_UNSIGNED_BYTE,img);
                    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
                    stbi_image_free(img); haveTestPattern=true; vlogln(std::string("Auto-loaded test pattern: ") + opt_test_pattern_path);
                }
            }
        }
    }

    glUseProgram(program);
    GLint loc_texY = glGetUniformLocation(program, "texY");
    GLint loc_texUV = glGetUniformLocation(program, "texUV");
    if (loc_texY >= 0) glUniform1i(loc_texY, 0);
    if (loc_texUV >= 0) glUniform1i(loc_texUV, 1);

    GLint loc_texPattern = glGetUniformLocation(program, "texPattern");
    GLint loc_u_showPattern = glGetUniformLocation(program, "u_showPattern");
    if (loc_texPattern >= 0) glUniform1i(loc_texPattern, 2);
    if (loc_u_showPattern >= 0) glUniform1i(loc_u_showPattern, 0);

    // central helper to set the pattern uniform in one place
    auto setPattern = [&](bool on) {
        if (!ENABLE_SHADER_TEST_PATTERN) return;
        if (loc_u_showPattern >= 0) {
            glUseProgram(program);
            glUniform1i(loc_u_showPattern, on ? 1 : 0);
        }
    };
    setPattern(false);

    GLint loc_uv_swap = glGetUniformLocation(program, "uv_swap");
    GLint loc_use_bt709 = glGetUniformLocation(program, "use_bt709");
    GLint loc_full_range = glGetUniformLocation(program, "full_range");
    GLint loc_view_mode = glGetUniformLocation(program, "view_mode");
    GLint loc_segmentIndex = glGetUniformLocation(program, "segmentIndex");
    GLint loc_offsetxy1 = glGetUniformLocation(program, "offsetxy1");
    GLint loc_textureIsFull = glGetUniformLocation(program, "u_textureIsFull");
    GLint loc_u_windowSize = glGetUniformLocation(program, "u_windowSize");
    GLint loc_u_outputSize = glGetUniformLocation(program, "u_outputSize");
    GLint loc_u_alignTopLeft = glGetUniformLocation(program, "u_alignTopLeft");
    GLint loc_rot = glGetUniformLocation(program, "rot");
    GLint loc_flip_x = glGetUniformLocation(program, "flip_x");
    GLint loc_flip_y = glGetUniformLocation(program, "flip_y");
    GLint loc_gap_count = glGetUniformLocation(program, "gap_count");
    GLint loc_gap_rows = glGetUniformLocation(program, "gap_rows");
    GLint loc_u_fullInputSize = glGetUniformLocation(program, "u_fullInputSize");
    GLint loc_u_segmentsX = glGetUniformLocation(program, "u_segmentsX");
    GLint loc_u_segmentsY = glGetUniformLocation(program, "u_segmentsY");
    GLint loc_u_subBlockSize = glGetUniformLocation(program, "u_subBlockSize");
    GLint loc_u_tileW = glGetUniformLocation(program, "u_tileW");
    GLint loc_u_tileH = glGetUniformLocation(program, "u_tileH");
    GLint loc_u_spacingX = glGetUniformLocation(program, "u_spacingX");
    GLint loc_u_spacingY = glGetUniformLocation(program, "u_spacingY");
    GLint loc_u_marginX = glGetUniformLocation(program, "u_marginX");
    GLint loc_u_numTilesPerRow = glGetUniformLocation(program, "u_numTilesPerRow");
    GLint loc_u_numTilesPerCol = glGetUniformLocation(program, "u_numTilesPerCol");
    GLint loc_inputTilesTopToBottom = glGetUniformLocation(program, "inputTilesTopToBottom");
    GLint loc_moduleSerials = glGetUniformLocation(program, "moduleSerials");

    int uv_swap = 0;
    if (opt_uv_swap_override >= 0) uv_swap = opt_uv_swap_override;
    else { if (cur_pixfmt == V4L2_PIX_FMT_NV21) uv_swap = 1; else if (cur_pixfmt == V4L2_PIX_FMT_NV12) uv_swap = 0; else uv_swap = 0; }
    if (opt_cpu_uv_swap) uv_swap = 0;
    if (loc_uv_swap >= 0) glUniform1i(loc_uv_swap, uv_swap);
    if (loc_use_bt709 >= 0) glUniform1i(loc_use_bt709, opt_use_bt709);
    if (loc_full_range >= 0) glUniform1i(loc_full_range, opt_full_range);
    if (loc_view_mode >= 0) glUniform1i(loc_view_mode, 0);

    ControlParams ctrl; loadControlIni("control_ini.txt", ctrl);
    std::array<std::string,3> modFiles = buildModuleFilenames(ctrl);
    std::vector<GLint> offsetData;
    glUseProgram(program);
    if (loc_u_fullInputSize >= 0) glUniform2f(loc_u_fullInputSize, ctrl.fullInputW, ctrl.fullInputH);
    if (loc_u_segmentsX >= 0) glUniform1i(loc_u_segmentsX, ctrl.segmentsX);
    if (loc_u_segmentsY >= 0) glUniform1i(loc_u_segmentsY, ctrl.segmentsY);
    if (loc_u_subBlockSize >= 0) glUniform2f(loc_u_subBlockSize, ctrl.subBlockW, ctrl.subBlockH);
    if (loc_u_tileW >= 0) glUniform1f(loc_u_tileW, ctrl.tileW);
    if (loc_u_tileH >= 0) glUniform1f(loc_u_tileH, ctrl.tileH);
    if (loc_u_spacingX >= 0) glUniform1f(loc_u_spacingX, ctrl.spacingX);
    if (loc_u_spacingY >= 0) glUniform1f(loc_u_spacingY, ctrl.spacingY);
    if (loc_u_marginX >= 0) glUniform1f(loc_u_marginX, ctrl.marginX);
    if (loc_u_numTilesPerRow >= 0) glUniform1i(loc_u_numTilesPerRow, ctrl.numTilesPerRow);
    if (loc_u_numTilesPerCol >= 0) glUniform1i(loc_u_numTilesPerCol, ctrl.numTilesPerCol);
    if (loc_inputTilesTopToBottom >= 0) glUniform1i(loc_inputTilesTopToBottom, ctrl.inputTilesTopToBottom);
    if (loc_moduleSerials >= 0) glUniform1iv(loc_moduleSerials, 3, ctrl.moduleSerials);

    if (loadOffsetsFromModuleFiles(modFiles, offsetData)) {
        if (loc_offsetxy1 >= 0 && offsetData.size() >= 150*2) { glUseProgram(program); glUniform2iv(loc_offsetxy1, 150, offsetData.data()); }
    }

    if (loc_textureIsFull >= 0) {
        int textureIsFullInitial = ((int)cur_width == (int)ctrl.fullInputW && (int)cur_height == (int)ctrl.fullInputH) ? 1 : 0;
        glUseProgram(program); glUniform1i(loc_textureIsFull, textureIsFullInitial);
    }

    if (loc_u_windowSize >= 0) glUniform2f(loc_u_windowSize, (float)win_w, (float)win_h);
    if (loc_u_outputSize >= 0) glUniform2f(loc_u_outputSize, ctrl.subBlockW, ctrl.subBlockH);
    if (loc_u_alignTopLeft >= 0) glUniform1i(loc_u_alignTopLeft, 1);

    int flip_x = 0, flip_y = 1, rotation = 0;
    if (loc_flip_x >= 0) { glUseProgram(program); glUniform1i(loc_flip_x, flip_x); }
    if (loc_flip_y >= 0) { glUseProgram(program); glUniform1i(loc_flip_y, flip_y); }
    if (loc_rot >= 0) { glUseProgram(program); glUniform1i(loc_rot, rotation); }

    int activeSegment = 1;
    if (loc_segmentIndex >= 0) { glUseProgram(program); glUniform1i(loc_segmentIndex, activeSegment); }

    const int GAP_ARRAY_SIZE = 8;
    int gap_count = 2;
    int gap_rows_arr[GAP_ARRAY_SIZE] = { 5,10,0,0,0,0,0,0 };
    if (loc_gap_count >= 0) { glUseProgram(program); glUniform1i(loc_gap_count, gap_count); }
    if (loc_gap_rows >= 0) { glUseProgram(program); glUniform1iv(loc_gap_rows, GAP_ARRAY_SIZE, gap_rows_arr); }

    const int POLL_TIMEOUT_MS = 200;
    const uint64_t CHECK_FMT_INTERVAL = 120;
    uint64_t frame_count = 0;
    struct pollfd pfd; pfd.fd = fd; pfd.events = POLLIN | POLLPRI;

    std::vector<unsigned char> tmpUVbuf, tmpFallback;
    std::vector<unsigned char> lastY, lastUV, lastPacked;
    size_t lastPackedSize = 0; int last_stride = 0; int last_width=0, last_height=0;
    bool lastIsNV12_NV21=false; uint32_t last_pixfmt=0;

    vlogln("startup: entering main loop");

    using clock = std::chrono::steady_clock;
    auto last_good_frame = clock::now();
    auto last_recovered_time = clock::time_point();

    bool signal_lost = false;
    // NEW: manual override to show test pattern with 't' (toggle)
    bool manual_show_pattern = false;

    int einval_count = 0;
    std::mutex restart_mutex;

    std::atomic<bool> auto_reopen_in_progress(false);
    std::atomic<bool> need_gl_update(false);

    // V4L2-only manual restart helper (no GL calls)
    auto manual_restart_v4l_only = [&]() -> bool {
        std::lock_guard<std::mutex> lk(restart_mutex);
        vlogln("manual_restart_v4l_only: attempting restart_v4l_stream()");
        if (fd >= 0) {
            if (restart_v4l_stream(fd, buffers)) {
                last_good_frame = clock::now();
                last_recovered_time = last_good_frame;
                einval_count = 0;
                signal_lost = false;
                need_gl_update.store(true, std::memory_order_release);
                vlogln("manual_restart_v4l_only: restart_v4l_stream succeeded (V4L2-only)");
                return true;
            }
            vlogln("manual_restart_v4l_only: restart_v4l_stream failed, will try full reopen");
        }
        if (fd >= 0) { close(fd); fd = -1; }
        const int OPEN_RETRIES = 10; const int OPEN_RETRY_MS = 200;
        int newfd = -1;
        for (int i=0;i<OPEN_RETRIES;++i) {
            newfd = open(DEVICE, O_RDWR | O_NONBLOCK);
            if (newfd >= 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(OPEN_RETRY_MS));
        }
        if (newfd < 0) { vlogln("manual_restart_v4l_only: open device failed"); return false; }
        fd = newfd;
        uint32_t nw=0, nh=0, npf=0;
        if (!get_v4l2_format(fd, nw, nh, npf)) { nw = cur_width; nh = cur_height; npf = cur_pixfmt; }
        // update shared format/state under mutex
        {
            std::lock_guard<std::mutex> lk2(restart_mutex);
            cur_width = nw; cur_height = nh; cur_pixfmt = npf;
        }
        v4l2_format sfmt; memset(&sfmt,0,sizeof(sfmt));
        sfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        sfmt.fmt.pix_mp.width = cur_width; sfmt.fmt.pix_mp.height = cur_height;
        sfmt.fmt.pix_mp.pixelformat = v4l2_fourcc('N','V','2','4');
        sfmt.fmt.pix_mp.field = V4L2_FIELD_NONE; sfmt.fmt.pix_mp.num_planes = 1;
        (void)xioctl(fd, VIDIOC_S_FMT, &sfmt);
        get_v4l2_format(fd, cur_width, cur_height, cur_pixfmt);

        v4l2_requestbuffers req2; memset(&req2,0,sizeof(req2));
        req2.count = BUF_COUNT; req2.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; req2.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_REQBUFS, &req2) < 0) { vlogln(std::string("Manual restart: VIDIOC_REQBUFS failed: ")+strerror(errno)); close(fd); fd=-1; return false; }
        if (req2.count == 0) { vlogln("Manual restart: VIDIOC_REQBUFS returned zero buffers"); close(fd); fd=-1; return false; }
        buffers.clear(); buffers.resize(req2.count);
        for (unsigned i=0;i<req2.count;++i) {
            v4l2_buffer bufq; v4l2_plane planes[VIDEO_MAX_PLANES]; memset(&bufq,0,sizeof(bufq)); memset(planes,0,sizeof(planes));
            bufq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; bufq.index = i; bufq.memory = V4L2_MEMORY_MMAP; bufq.m.planes = planes; bufq.length = VIDEO_MAX_PLANES;
            if (xioctl(fd, VIDIOC_QUERYBUF, &bufq) < 0) {
                vlogln(std::string("Manual restart: VIDIOC_QUERYBUF failed: ") + strerror(errno));
                for (auto &bvec : buffers) for (auto &pm : bvec) if (pm.addr && pm.length) munmap(pm.addr, pm.length);
                buffers.clear(); close(fd); fd=-1; return false;
            }
            buffers[i].resize(bufq.length);
            for (unsigned p=0;p<bufq.length;++p) {
                buffers[i][p].length = planes[p].length;
                buffers[i][p].addr = mmap(nullptr, planes[p].length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, planes[p].m.mem_offset);
                if (buffers[i][p].addr == MAP_FAILED) {
                    vlogln(std::string("Manual restart: mmap failed: ") + strerror(errno));
                    for (auto &bvec : buffers) for (auto &pm : bvec) if (pm.addr && pm.length) munmap(pm.addr, pm.length);
                    buffers.clear(); close(fd); fd=-1; return false;
                }
            }
            if (xioctl(fd, VIDIOC_QBUF, &bufq) < 0) {
                vlogln(std::string("Manual restart: VIDIOC_QBUF failed: ") + strerror(errno));
                for (auto &bvec : buffers) for (auto &pm : bvec) if (pm.addr && pm.length) munmap(pm.addr, pm.length);
                buffers.clear(); close(fd); fd=-1; return false;
            }
        }
        int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) {
            vlogln(std::string("Manual restart: VIDIOC_STREAMON failed: ") + strerror(errno));
            for (auto &bvec : buffers) for (auto &pm : bvec) if (pm.addr && pm.length) munmap(pm.addr, pm.length);
            buffers.clear(); close(fd); fd=-1; return false;
        }
        last_good_frame = clock::now(); last_recovered_time = last_good_frame; einval_count = 0; signal_lost = false;
        need_gl_update.store(true, std::memory_order_release);
        vlogln("manual_restart_v4l_only: full reopen succeeded (V4L2-only)");
        return true;
    };

    // Background reopen starter: spawns thread that does V4L2-only restart + verification
    auto start_background_reopen = [&]() {
        bool expected = false;
        if (!auto_reopen_in_progress.compare_exchange_strong(expected, true)) return;
        std::thread([&]() {
            vlogln("Background reopen thread started");
            int attempt = 0;
            while (auto_reopen_in_progress.load()) {
                attempt++;
                // Try to perform V4L2-only restart
                bool restart_ok = manual_restart_v4l_only();
                if (!restart_ok) {
                    vlogln(std::string("Background reopen: V4L2 restart failed, retrying in 1000ms (attempt ") + std::to_string(attempt) + ")");
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000 + std::min(attempt*500, 5000)));
                    continue;
                }
                // After restart succeeded, verify we can actually dequeue a frame.
                bool verified = false;
                for (int v=0; v < BG_REOPEN_VERIFY_MAX_ATTEMPTS; ++v) {
                    // Poll fd for input
                    struct pollfd vpfd; vpfd.fd = fd; vpfd.events = POLLIN;
                    int pres = poll(&vpfd, 1, BG_REOPEN_VERIFY_POLL_MS);
                    if (pres > 0 && (vpfd.revents & POLLIN)) {
                        std::lock_guard<std::mutex> lk(restart_mutex);
                        v4l2_buffer buf; v4l2_plane planes[VIDEO_MAX_PLANES]; memset(&buf,0,sizeof(buf)); memset(planes,0,sizeof(planes));
                        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; buf.memory = V4L2_MEMORY_MMAP; buf.m.planes = planes; buf.length = VIDEO_MAX_PLANES;
                        int dq = ioctl(fd, VIDIOC_DQBUF, &buf);
                        if (dq == 0) {
                            size_t bytesused0 = planes[0].bytesused;
                            if (bytesused0 > 0) {
                                // Got a valid frame — requeue and accept
                                if (ioctl(fd, VIDIOC_QBUF, &buf) == 0) {
                                    verified = true;
                                    last_good_frame = clock::now();
                                    last_recovered_time = last_good_frame;
                                    einval_count = 0;
                                    vlogln("Background reopen: successfully dequeued+requeued a frame -> verified");
                                    break;
                                } else {
                                    // QBUF failed: try again
                                    vlogln(std::string("Background reopen: QBUF after DQBUF failed: ") + strerror(errno));
                                }
                            } else {
                                // bytesused==0: requeue and continue
                                if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
                                    vlogln(std::string("Background reopen: QBUF after empty frame failed: ") + strerror(errno));
                                }
                            }
                        } else {
                            // DQBUF failed; continue waiting
                            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                vlogln(std::string("Background reopen: VIDIOC_DQBUF failed during verify: ") + strerror(errno));
                            }
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                } // end verify loop

                if (verified) {
                    need_gl_update.store(true, std::memory_order_release);
                    vlogln("Background reopen: verified, signalling GL thread to reinit textures");
                    break;
                } else {
                    vlogln("Background reopen: verification failed after restart, will retry full reopen");
                    std::this_thread::sleep_for(std::chrono::milliseconds(800));
                    // continue loop to attempt reopen again
                }
            }
            auto_reopen_in_progress.store(false);
            vlogln("Background reopen thread exiting");
        }).detach();
    };

    // The main loop:
    while (true) {
      int ret = poll(&pfd, 1, POLL_TIMEOUT_MS);
      if (ret < 0) { if (errno == EINTR) continue; perror("poll"); break; }

      // If background reopen succeeded and wants GL update: do it here (GL thread)
      if (need_gl_update.load(std::memory_order_acquire)) {
          std::lock_guard<std::mutex> lk(restart_mutex);
          int new_uv_w = (cur_pixfmt == V4L2_PIX_FMT_NV12 || cur_pixfmt == V4L2_PIX_FMT_NV21) ? (int)(cur_width/2) : (int)cur_width;
          int new_uv_h = (cur_pixfmt == V4L2_PIX_FMT_NV12 || cur_pixfmt == V4L2_PIX_FMT_NV21) ? (int)(cur_height/2) : (int)cur_height;
          reallocate_textures(texY, texUV, (int)cur_width, (int)cur_height, new_uv_w, new_uv_h);
          if (opt_auto_resize_window) SDL_SetWindowSize(win, (int)cur_width, (int)cur_height);
          if (opt_uv_swap_override < 0 && !opt_cpu_uv_swap) {
              int old_uv = uv_swap;
              if (cur_pixfmt == V4L2_PIX_FMT_NV21) uv_swap = 1;
              else if (cur_pixfmt == V4L2_PIX_FMT_NV12) uv_swap = 0;
              if (uv_swap != old_uv && loc_uv_swap >= 0) { glUseProgram(program); glUniform1i(loc_uv_swap, uv_swap); }
          }
          if (loc_textureIsFull >= 0) {
              int textureIsFull = ((int)cur_width == (int)ctrl.fullInputW && (int)cur_height == (int)ctrl.fullInputH) ? 1 : 0;
              glUseProgram(program); glUniform1i(loc_textureIsFull, textureIsFull);
          }
          // Hide test pattern now that GL textures are ready
          signal_lost = false;
          setPattern(false);
          need_gl_update.store(false, std::memory_order_release);
          vlogln("Main thread: completed GL reinit after background reopen");
      }

      // If background reopen in progress, skip DQBUF handling (avoid races)
      if (auto_reopen_in_progress.load()) {
          // still process events and render pattern (so user sees pattern while reopening)
      } else {
          // Normal DQBUF handling (same as before)
          v4l2_buffer buf; v4l2_plane planes[VIDEO_MAX_PLANES];
          memset(&buf,0,sizeof(buf)); memset(planes,0,sizeof(planes));
          buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; buf.memory = V4L2_MEMORY_MMAP; buf.m.planes = planes; buf.length = VIDEO_MAX_PLANES;

          bool dequeued = false;
          auto now = clock::now();
          if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
            int e = errno;
            if (e == EAGAIN || e == EWOULDBLOCK) {
                // no frame
            } else if (e == EINVAL) {
                einval_count++;
                signal_lost = true; setPattern(true);
                vlogln(std::string("VIDIOC_DQBUF returned EINVAL -> immediate signal_lost (count=") + std::to_string(einval_count) + ")");
                if (einval_count >= EINVAL_RESTART_THRESHOLD) {
                    bool restarted=false;
                    for (int r=0;r<STREAM_RESTART_RETRIES;++r) {
                        std::lock_guard<std::mutex> lk(restart_mutex);
                        if (restart_v4l_stream(fd, buffers)) { restarted=true; break; }
                        std::this_thread::sleep_for(std::chrono::milliseconds(STREAM_RESTART_BACKOFF_MS));
                    }
                    if (restarted) {
                        einval_count=0; last_good_frame = clock::now(); last_recovered_time = clock::now();
                        signal_lost=false; setPattern(false);
                        vlogln("Stream restart succeeded after EINVALs");
                    } else {
                        vlogln("Stream restart failed after repeated EINVALs; starting background full reopen attempts");
                        start_background_reopen();
                    }
                } else {
                    // start background reopen early to be more responsive
                    start_background_reopen();
                }
            } else {
                vlogln(std::string("VIDIOC_DQBUF non-fatal failure: ") + strerror(e));
            }
          } else {
            dequeued = true;
            unsigned char* base = (unsigned char*)buffers[buf.index][0].addr;
            size_t bytesused0 = planes[0].bytesused;
            size_t Y_len = (size_t)cur_width * (size_t)cur_height;
            size_t UV_len = 0;
            bool isNV12_NV21 = (cur_pixfmt == V4L2_PIX_FMT_NV12 || cur_pixfmt == V4L2_PIX_FMT_NV21);
            if (isNV12_NV21) UV_len = (size_t)cur_width * ((size_t)cur_height / 2);
            else UV_len = (size_t)cur_width * (size_t)cur_height * 2;
            size_t total_expected = Y_len + UV_len;

            if (bytesused0 == 0) {
                signal_lost = true; setPattern(true);
                vlogln("Dequeued buffer with bytesused==0 -> immediate signal_lost");
                // start background reopen to try to recover
                start_background_reopen();
            }

            unsigned char* ybase=nullptr; unsigned char* uvbase=nullptr;
            if (buf.length >= 2 && buffers[buf.index].size() >= 2) {
                ybase = (unsigned char*)buffers[buf.index][0].addr; uvbase = (unsigned char*)buffers[buf.index][1].addr;
            } else if (bytesused0 >= total_expected) {
                ybase = base; uvbase = base + Y_len;
            } else { ybase = base; uvbase = nullptr; }

            if (ybase && !signal_lost) {
                if ((int)cur_width <= gl_max_tex && (int)cur_height <= gl_max_tex) {
                    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, texY);
                    glPixelStorei(GL_UNPACK_ALIGNMENT,1); glTexSubImage2D(GL_TEXTURE_2D,0,0,0,(int)cur_width,(int)cur_height,GL_RED,GL_UNSIGNED_BYTE,ybase);
                } else upload_texture_tiled(GL_RED, texY, (int)cur_width, (int)cur_height, ybase, gl_max_tex, 1);

                last_pixfmt = cur_pixfmt;
                if (isNV12_NV21 && ybase && uvbase) {
                    size_t ylen = (size_t)cur_width * (size_t)cur_height; size_t uvlen = (size_t)cur_width * ((size_t)cur_height / 2);
                    if (lastY.size() < ylen) lastY.resize(ylen);
                    if (lastUV.size() < uvlen) lastUV.resize(uvlen);
                    memcpy(lastY.data(), ybase, ylen); memcpy(lastUV.data(), uvbase, uvlen);
                    last_width = (int)cur_width; last_height = (int)cur_height; lastIsNV12_NV21 = true;
                    lastPacked.clear(); lastPackedSize=0; last_stride=0;
                } else {
                    if (uvbase==nullptr) {
                        unsigned char* packedPtr = base; size_t packedSize = bytesused0>0?bytesused0:(size_t)cur_width*(size_t)cur_height*2;
                        if (lastPacked.size() < packedSize) lastPacked.resize(packedSize);
                        memcpy(lastPacked.data(), packedPtr, packedSize);
                        lastPackedSize=packedSize; last_stride=(int)cur_width*2; last_width=(int)cur_width; last_height=(int)cur_height; lastIsNV12_NV21=false;
                    } else {
                        size_t ylen=(size_t)cur_width*(size_t)cur_height; size_t uvlen=(size_t)cur_width*((size_t)cur_height/2);
                        size_t packedSize = ylen + uvlen;
                        if (lastPacked.size() < packedSize) lastPacked.resize(packedSize);
                        memcpy(lastPacked.data(), ybase, ylen); memcpy(lastPacked.data()+ylen, uvbase, uvlen);
                        lastPackedSize=packedSize; last_width=(int)cur_width; last_height=(int)cur_height; lastIsNV12_NV21=false;
                    }
                }
            }

            if (uvbase && !signal_lost) {
                int upload_w = isNV12_NV21 ? (int)(cur_width/2) : (int)cur_width;
                int upload_h = isNV12_NV21 ? (int)(cur_height/2) : (int)cur_height;
                if (opt_cpu_uv_swap && cur_pixfmt == V4L2_PIX_FMT_NV21) {
                    size_t need = (size_t)upload_w*(size_t)upload_h*2; if (tmpUVbuf.size() < need) tmpUVbuf.resize(need);
                    unsigned char* dst = tmpUVbuf.data();
                    if (isNV12_NV21) {
                        for (int y=0;y<upload_h;++y) {
                            const unsigned char* srcRow = uvbase + (size_t)y*(size_t)cur_width;
                            unsigned char* dstRow = dst + (size_t)y*(size_t)upload_w*2;
                            for (int x=0;x<upload_w;++x) { unsigned char v = srcRow[x*2+0]; unsigned char u = srcRow[x*2+1]; dstRow[x*2+0]=u; dstRow[x*2+1]=v; }
                        }
                    } else {
                        for (int y=0;y<upload_h;++y) {
                            const unsigned char* srcRow = uvbase + (size_t)y*(size_t)upload_w*2;
                            unsigned char* dstRow = dst + (size_t)y*(size_t)upload_w*2;
                            for (int x=0;x<upload_w;++x) { unsigned char v = srcRow[x*2+0]; unsigned char u = srcRow[x*2+1]; dstRow[x*2+0]=u; dstRow[x*2+1]=v; }
                        }
                    }
                    if (upload_w <= gl_max_tex && upload_h <= gl_max_tex) {
                        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, texUV);
                        glPixelStorei(GL_UNPACK_ALIGNMENT,1); glTexSubImage2D(GL_TEXTURE_2D,0,0,0,upload_w,upload_h,GL_RG,GL_UNSIGNED_BYTE,tmpUVbuf.data());
                    } else upload_texture_tiled(GL_RG, texUV, upload_w, upload_h, tmpUVbuf.data(), gl_max_tex, 2);
                } else {
                    if (upload_w <= gl_max_tex && upload_h <= gl_max_tex) {
                        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, texUV);
                        glPixelStorei(GL_UNPACK_ALIGNMENT,1); glTexSubImage2D(GL_TEXTURE_2D,0,0,0,upload_w,upload_h,GL_RG,GL_UNSIGNED_BYTE,uvbase);
                    } else upload_texture_tiled(GL_RG, texUV, upload_w, upload_h, uvbase, gl_max_tex, 2);
                }
            }

            // requeue buffer
            bool qbuf_ok=false;
            for (int attempt=0; attempt < QBUF_RETRIES; ++attempt) {
                if (xioctl(fd, VIDIOC_QBUF, &buf) == 0) { qbuf_ok=true; if (opt_verbose && attempt>0) vlogln(std::string("VIDIOC_QBUF succeeded after ") + std::to_string(attempt) + " retries"); break; }
                else { int e = errno; if (opt_verbose) vlogln(std::string("VIDIOC_QBUF failed (attempt ") + std::to_string(attempt+1) + "): " + strerror(e)); usleep(QBUF_RETRY_MS*1000); }
            }
            if (!qbuf_ok) std::cerr<<"VIDIOC_QBUF failed after retries; will fallback to pattern if no subsequent good frames\n";
            else {
                last_good_frame = now; last_recovered_time = now; einval_count = 0;
                if (signal_lost) { signal_lost = false; setPattern(false); vlogln("Recovered to live (QBUF success)"); }
            }
          } // end normal DQBUF handling
      }

      // Check SOURCE_CHANGE events if any
      if (pfd.revents & POLLPRI) {
          v4l2_event ev;
          while (ioctl(fd, VIDIOC_DQEVENT, &ev) == 0) {
              if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
                  uint32_t new_w=0,new_h=0,new_pf=0;
                  bool got = get_v4l2_format(fd, new_w, new_h, new_pf);
                  if (!got || new_w==0 || new_h==0) { signal_lost=true; setPattern(true); vlogln("SOURCE_CHANGE: invalid format -> signal_lost"); start_background_reopen(); }
                  else {
                      std::lock_guard<std::mutex> lk(restart_mutex);
                      if (new_w != cur_width || new_h != cur_height || new_pf != cur_pixfmt) {
                          cur_width=new_w; cur_height=new_h; cur_pixfmt=new_pf;
                          int new_uv_w = (cur_pixfmt == V4L2_PIX_FMT_NV12 || cur_pixfmt == V4L2_PIX_FMT_NV21) ? (int)(cur_width/2) : (int)cur_width;
                          int new_uv_h = (cur_pixfmt == V4L2_PIX_FMT_NV12 || cur_pixfmt == V4L2_PIX_FMT_NV21) ? (int)(cur_height/2) : (int)cur_height;
                          reallocate_textures(texY, texUV, (int)cur_width, (int)cur_height, new_uv_w, new_uv_h);
                          if (opt_auto_resize_window) SDL_SetWindowSize(win, (int)cur_width, (int)cur_height);
                          if (opt_uv_swap_override < 0 && !opt_cpu_uv_swap) {
                              int old_uv = uv_swap;
                              if (cur_pixfmt == V4L2_PIX_FMT_NV21) uv_swap = 1; else if (cur_pixfmt == V4L2_PIX_FMT_NV12) uv_swap = 0;
                              if (uv_swap != old_uv && loc_uv_swap >= 0) { glUseProgram(program); glUniform1i(loc_uv_swap, uv_swap); }
                          }
                          if (loc_textureIsFull >= 0) {
                              int textureIsFull = ((int)cur_width == (int)ctrl.fullInputW && (int)cur_height == (int)ctrl.fullInputH) ? 1 : 0;
                              glUseProgram(program); glUniform1i(loc_textureIsFull, textureIsFull);
                          }
                      }
                  }
              }
          }
      }

      // Timeout -> set pattern if no good frames recently (respect recovery grace)
      auto now2 = clock::now();
      auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now2 - last_good_frame).count();
      if (!signal_lost && elapsedMs > PATTERN_TIMEOUT_MS) {
          bool within_recovery_grace = false;
          if (last_recovered_time != clock::time_point()) {
              auto since_recovered = std::chrono::duration_cast<std::chrono::milliseconds>(now2 - last_recovered_time).count();
              if (since_recovered < RECOVERY_GRACE_MS) within_recovery_grace = true;
          }
          if (!within_recovery_grace) {
              signal_lost = true; vlogln("signal_lost: timeout reached -> showing pattern"); setPattern(true);
              start_background_reopen();
          } else if (opt_verbose) {
              vlogln(std::string("Skipping signal_lost due to recovery grace (") + std::to_string(elapsedMs) + "ms since last_good_frame)");
          }
      }

      // Render
      glClear(GL_COLOR_BUFFER_BIT);
      glUseProgram(program);
      if (loc_rot >= 0) glUniform1i(loc_rot, rotation);
      if (loc_flip_x >= 0) glUniform1i(loc_flip_x, flip_x);
      if (loc_flip_y >= 0) glUniform1i(loc_flip_y, flip_y);
      if (loc_gap_count >= 0) glUniform1i(loc_gap_count, gap_count);
      if (loc_gap_rows >= 0) glUniform1iv(loc_gap_rows, GAP_ARRAY_SIZE, gap_rows_arr);
      if (loc_u_fullInputSize >= 0) glUniform2f(loc_u_fullInputSize, ctrl.fullInputW, ctrl.fullInputH);
      if (loc_u_segmentsX >= 0) glUniform1i(loc_u_segmentsX, ctrl.segmentsX);
      if (loc_u_segmentsY >= 0) glUniform1i(loc_u_segmentsY, ctrl.segmentsY);
      if (loc_u_subBlockSize >= 0) glUniform2f(loc_u_subBlockSize, ctrl.subBlockW, ctrl.subBlockH);
      if (loc_u_tileW >= 0) glUniform1f(loc_u_tileW, ctrl.tileW);
      if (loc_u_tileH >= 0) glUniform1f(loc_u_tileH, ctrl.tileH);
      if (loc_u_spacingX >= 0) glUniform1f(loc_u_spacingX, ctrl.spacingX);
      if (loc_u_spacingY >= 0) glUniform1f(loc_u_spacingY, ctrl.spacingY);
      if (loc_u_marginX >= 0) glUniform1f(loc_u_marginX, ctrl.marginX);
      if (loc_u_numTilesPerRow >= 0) glUniform1i(loc_u_numTilesPerRow, ctrl.numTilesPerRow);
      if (loc_u_numTilesPerCol >= 0) glUniform1i(loc_u_numTilesPerCol, ctrl.numTilesPerCol);
      if (loc_inputTilesTopToBottom >= 0) glUniform1i(loc_inputTilesTopToBottom, ctrl.inputTilesTopToBottom);
      if (loc_moduleSerials >= 0) glUniform1iv(loc_moduleSerials, 3, ctrl.moduleSerials);
      if (!opt_cpu_uv_swap && loc_uv_swap >= 0) glUniform1i(loc_uv_swap, uv_swap);
      if (loc_use_bt709 >= 0) glUniform1i(loc_use_bt709, opt_use_bt709);
      if (loc_full_range >= 0) glUniform1i(loc_full_range, opt_full_range);
      if (loc_textureIsFull >= 0) {
          int textureIsFull = ((int)cur_width == (int)ctrl.fullInputW && (int)cur_height == (int)ctrl.fullInputH) ? 1 : 0;
          glUniform1i(loc_textureIsFull, textureIsFull);
      }
      if (loc_u_windowSize >= 0) glUniform2f(loc_u_windowSize, (float)win_w, (float)win_h);
      if (loc_u_outputSize >= 0) glUniform2f(loc_u_outputSize, ctrl.subBlockW, ctrl.subBlockH);
      if (loc_u_alignTopLeft >= 0) glUniform1i(loc_u_alignTopLeft, 1);
      if (loc_segmentIndex >= 0) {
          int maxSeg = std::max(1, ctrl.segmentsX * ctrl.segmentsY);
          int segToSet = std::min(std::max(1, activeSegment), maxSeg);
          glUniform1i(loc_segmentIndex, segToSet);
      }

      // Updated: show pattern if signal_lost OR manual_show_pattern (toggle with 't')
      int showPatternFlag = (ENABLE_SHADER_TEST_PATTERN && (signal_lost || manual_show_pattern)) ? 1 : 0;
      if (loc_u_showPattern >= 0) glUniform1i(loc_u_showPattern, showPatternFlag);

      if (haveTestPattern && ENABLE_SHADER_TEST_PATTERN) { glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, texPattern); }
      glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, texY);
      glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, texUV);
      glBindVertexArray(vao); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      SDL_GL_SwapWindow(win);

      // SDL events
      SDL_Event e;
      while (SDL_PollEvent(&e)) {
          if (e.type == SDL_QUIT) { goto shutdown; }
          if (e.type == SDL_WINDOWEVENT) {
              if (e.window.event == SDL_WINDOWEVENT_RESIZED || e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                  SDL_GetWindowSize(win, &win_w, &win_h);
                  glViewport(0, 0, win_w, win_h);
                  if (loc_u_windowSize >= 0) {
                      glUseProgram(program);
                      glUniform2f(loc_u_windowSize, (float)win_w, (float)win_h);
                      if (loc_u_alignTopLeft >= 0) glUniform1i(loc_u_alignTopLeft, 1);
                  }
              }
          }
          else if (e.type == SDL_KEYDOWN) {
              SDL_Keycode k = e.key.keysym.sym;
              if (k == SDLK_ESCAPE) { goto shutdown; }
              else if (k == SDLK_f) {
                  if (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) SDL_SetWindowFullscreen(win,0);
                  else SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
                  SDL_GetWindowSize(win,&win_w,&win_h); glViewport(0,0,win_w,win_h);
                  if (loc_u_windowSize >= 0) { glUseProgram(program); glUniform2f(loc_u_windowSize, (float)win_w, (float)win_h); if (loc_u_alignTopLeft >= 0) glUniform1i(loc_u_alignTopLeft,1); }
              } else if (k == SDLK_k) {
                  ControlParams newCtrl; if (loadControlIni("control_ini.txt", newCtrl)) {
                      ctrl = newCtrl; glUseProgram(program);
                      if (loc_u_fullInputSize >= 0) glUniform2f(loc_u_fullInputSize, ctrl.fullInputW, ctrl.fullInputH);
                      if (loc_u_segmentsX >= 0) glUniform1i(loc_u_segmentsX, ctrl.segmentsX);
                      if (loc_u_segmentsY >= 0) glUniform1i(loc_u_segmentsY, ctrl.segmentsY);
                      if (loc_u_subBlockSize >= 0) glUniform2f(loc_u_subBlockSize, ctrl.subBlockW, ctrl.subBlockH);
                      if (loc_u_tileW >= 0) glUniform1f(loc_u_tileW, ctrl.tileW);
                      if (loc_u_tileH >= 0) glUniform1f(loc_u_tileH, ctrl.tileH);
                      if (loc_u_spacingX >= 0) glUniform1f(loc_u_spacingX, ctrl.spacingX);
                      if (loc_u_spacingY >= 0) glUniform1f(loc_u_spacingY, ctrl.spacingY);
                      if (loc_u_marginX >= 0) glUniform1f(loc_u_marginX, ctrl.marginX);
                      if (loc_u_numTilesPerRow >= 0) glUniform1i(loc_u_numTilesPerRow, ctrl.numTilesPerRow);
                      if (loc_u_numTilesPerCol >= 0) glUniform1i(loc_u_numTilesPerCol, ctrl.numTilesPerCol);
                      if (loc_inputTilesTopToBottom >= 0) glUniform1i(loc_inputTilesTopToBottom, ctrl.inputTilesTopToBottom);
                      if (loc_moduleSerials >= 0) glUniform1iv(loc_moduleSerials, 3, ctrl.moduleSerials);
                      modFiles = buildModuleFilenames(ctrl);
                      std::vector<GLint> newOffsets; if (loadOffsetsFromModuleFiles(modFiles, newOffsets)) {
                          if (loc_offsetxy1 >= 0 && newOffsets.size() >= 150*2) { glUseProgram(program); glUniform2iv(loc_offsetxy1, 150, newOffsets.data()); offsetData.swap(newOffsets); }
                      }
                  }
              } else if (k == SDLK_h) { flip_x = !flip_x; if (loc_flip_x >= 0) { glUseProgram(program); glUniform1i(loc_flip_x, flip_x); } }
              else if (k == SDLK_v) { flip_y = !flip_y; if (loc_flip_y >= 0) { glUseProgram(program); glUniform1i(loc_flip_y, flip_y); } }
              else if (k == SDLK_r) { rotation = (rotation + 2) & 3; if (loc_rot >= 0) { glUseProgram(program); glUniform1i(loc_rot, rotation); } }
              else if (k == SDLK_o) {
                  vlogln("User requested manual restart (key 'o')");
                  start_background_reopen(); // reuse same v4l-only path but triggered immediately
              } else if (k == SDLK_t) {
                  // NEW: toggle manual test pattern override
                  manual_show_pattern = !manual_show_pattern;
                  vlogln(std::string("Manual test-pattern toggle: ") + (manual_show_pattern ? "ON" : "OFF"));
                  // update shader immediately as well (keeps UI responsive)
                  setPattern(manual_show_pattern || signal_lost);
              } else if (k == SDLK_1 || k == SDLK_2 || k == SDLK_3) {
                  int requested = -1; if (k==SDLK_1) requested=1; if (k==SDLK_2) requested=2; if (k==SDLK_3) requested=3;
                  if (requested>0) { int maxSeg = std::max(1, ctrl.segmentsX * ctrl.segmentsY); if (requested <= maxSeg) { activeSegment = requested; if (loc_segmentIndex >= 0) { glUseProgram(program); glUniform1i(loc_segmentIndex, activeSegment); } } }
              } else if (k == SDLK_s) {
                  if (last_width>0 && last_height>0) {
                      std::vector<unsigned char> copyY, copyUV, copyPacked;
                      if (lastIsNV12_NV21) { copyY = lastY; copyUV = lastUV; } else copyPacked = lastPacked;
                      int w = last_width, h = last_height; uint32_t fmt = last_pixfmt;
                      int uv_swap_flag = uv_swap; int use_bt709_flag = opt_use_bt709; int full_range_flag = opt_full_range;
                      std::thread t(async_save_frame_to_png, std::move(copyY), std::move(copyUV), std::move(copyPacked), lastPackedSize, w, h, fmt, uv_swap_flag, use_bt709_flag, full_range_flag, std::string("display.png"));
                      t.detach();
                  }
              }
          }
      } // end event handling
      ++frame_count;
    } // end main loop

shutdown:
    if (texPattern) glDeleteTextures(1,&texPattern);
    glDeleteTextures(1,&texY); glDeleteTextures(1,&texUV);
    glDeleteBuffers(1,&vbo); glDeleteVertexArrays(1,&vao); glDeleteProgram(program);
    SDL_GL_DeleteContext(glc); SDL_DestroyWindow(win); SDL_Quit();
    for (auto &bufv : buffers) for (auto &pm : bufv) if (pm.addr && pm.length) munmap(pm.addr, pm.length);
    close(fd);
    vlogln("shutdown: normal exit");
    return 0;
}
