// hdmi_simple_display.cpp
// Production: auto-resize, tiled uploads, V4L2 events, CLI, CPU UV-swap option, improved tiled upload reuse
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

#define DEVICE "/dev/video0"
#define DEFAULT_WIDTH 1920
#define DEFAULT_HEIGHT 1080
#define WINDOW_TITLE "hdmi_simple_display (OpenGL YUV Shader)"
#define BUF_COUNT 4 // MMAP buffer count

static bool opt_auto_resize_window = false;
static bool opt_cpu_uv_swap = false; // if true, do CPU swap at upload
static int opt_uv_swap_override = -1; // -1 = auto, 0/1 override
static int opt_full_range = 0; // 0 limited, 1 full
static int opt_use_bt709 = 1; // 1 = BT.709, 0 = BT.601

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
    GLint status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        std::string msg;
        msg.resize(16384);
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
    GLint status;
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (!status) {
        std::string msg;
        msg.resize(16384);
        glGetProgramInfoLog(prog, (GLsizei)msg.size(), nullptr, &msg[0]);
        std::cerr << "Shader link failed: " << msg << std::endl;
        exit(EXIT_FAILURE);
    }
    glDeleteShader(vert);
    glDeleteShader(frag);
    return prog;
}

struct PlaneMap { void* addr; size_t length; };

std::string fourcc_to_str(uint32_t f) {
    char s[5] = { (char)(f & 0xFF), (char)((f>>8)&0xFF), (char)((f>>16)&0xFF), (char)((f>>24)&0xFF), 0 };
    return std::string(s);
}

// Query current V4L2 format (width/height/pixelformat)
bool get_v4l2_format(int fd, uint32_t &width, uint32_t &height, uint32_t &pixelformat) {
    v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
        return false;
    }
    width = fmt.fmt.pix_mp.width;
    height = fmt.fmt.pix_mp.height;
    pixelformat = fmt.fmt.pix_mp.pixelformat;
    return true;
}

// Reallocate GL textures for new width/height and uv size (must be called in GL context)
void reallocate_textures(GLuint texY, GLuint texUV, int newW, int newH, int uvW, int uvH) {
    glBindTexture(GL_TEXTURE_2D, texY);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, newW, newH, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

    glBindTexture(GL_TEXTURE_2D, texUV);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, uvW, uvH, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
}

// Improved tiled upload: reuse a single tile buffer to avoid repeated allocations
void upload_texture_tiled(GLenum format, GLuint tex, int srcW, int srcH,
                          const unsigned char* src, int maxTexSize, int pixelSizePerTexel) {
    // tile dims
    int tileW = std::min(srcW, maxTexSize);
    int tileH = std::min(srcH, maxTexSize);

    static std::vector<unsigned char> tileBuf; // reused across calls

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
                // prepare contiguous tile buffer
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
              << "  --uv-swap=auto|0|1         (default auto) auto detect NV12/NV21 or override\n"
              << "  --range=limited|full       (default limited)\n"
              << "  --matrix=709|601           (default 709)\n"
              << "  --auto-resize-window       resize SDL window on format change\n"
              << "  --cpu-uv-swap              perform UV swap on CPU at upload and avoid runtime shader swap\n"
              << "  -h, --help                 show this help\n";
}

// --- Helpers: getExecutableDir(), fileExists(), findShaderFile() ---
static std::string getExecutableDir() {
    char* base = SDL_GetBasePath();
    if (base) {
        std::string dir(base);
        SDL_free(base);
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir.push_back('/');
        return dir;
    }
#if defined(__linux__)
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        std::string path(buf);
        auto pos = path.find_last_of('/');
        if (pos != std::string::npos) return path.substr(0, pos + 1);
    }
#endif
    return std::string("./");
}

static bool fileExists(const std::string &path) {
    struct stat sb;
    return (stat(path.c_str(), &sb) == 0 && S_ISREG(sb.st_mode));
}

static std::string findShaderFile(const std::string &name, std::vector<std::string>* outAttempts = nullptr) {
    if (name.empty()) return std::string();
    std::vector<std::string> candidates;
    candidates.push_back(name);
    std::string exeDir = getExecutableDir();
    if (!exeDir.empty()) {
        candidates.push_back(exeDir + name);
        candidates.push_back(exeDir + "shaders/" + name);
        candidates.push_back(exeDir + "../" + name);
        candidates.push_back(exeDir + "../shaders/" + name);
        candidates.push_back(exeDir + "../../shaders/" + name);
        candidates.push_back(exeDir + "assets/" + name);
    }
    candidates.push_back(std::string("shaders/") + name);
    candidates.push_back(std::string("/usr/local/share/hdmi-in-display/shaders/") + name);
    candidates.push_back(std::string("/usr/share/hdmi-in-display/shaders/") + name);

    if (outAttempts) {
        outAttempts->clear();
        outAttempts->reserve(candidates.size());
    }
    for (const auto &p : candidates) {
        if (outAttempts) outAttempts->push_back(p);
        if (fileExists(p)) return p;
    }
    return std::string();
}

// --- Helpers for loading module offset files (robust, with logging) ---
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

// names: {"modul1.txt","modul2.txt","modul3.txt"}
// out: vector<GLint> of size 150*2 (x0,y0,x1,y1,...)
static bool loadOffsetsFromModuleFiles(const std::array<std::string,3> &names, std::vector<GLint> &out) {
    out.assign(150 * 2, 0);
    size_t fillIndex = 0;
    std::string exeDir = getExecutableDir();
    for (size_t m = 0; m < names.size(); ++m) {
        std::string candidates[2] = { joinPath(exeDir, names[m]), names[m] };
        bool fileFound = false;
        for (int c = 0; c < 2; ++c) {
            const std::string &path = candidates[c];
            std::cerr << "Trying open offsets file: " << path << "\n";
            if (!fileExists(path)) {
                std::cerr << "  not found: " << path << "\n";
                continue;
            }
            std::ifstream f(path);
            if (!f.is_open()) {
                std::cerr << "  cannot open: " << path << "\n";
                continue;
            }
            fileFound = true;
            size_t readThisFile = 0;
            std::string line;
            while (std::getline(f, line) && fillIndex < out.size()) {
                int x,y;
                if (!parseXYLine(line, x, y)) continue;
                out[fillIndex++] = (GLint)x;
                out[fillIndex++] = (GLint)y;
                ++readThisFile;
            }
            std::cerr << "  read " << readThisFile << " entries from " << path << "\n";
            break;
        }
        if (!fileFound) {
            std::cerr << "  WARNING: module file '" << names[m] << "' not found in exeDir or cwd; filling with 0s for these slots\n";
        }
    }
    std::cerr << "Total entries filled: " << (fillIndex/2) << " (out of 150)\n";
    return true;
}

int main(int argc, char** argv) {
  static struct option longopts[] = {
    {"uv-swap", required_argument, nullptr, 0},
    {"range", required_argument, nullptr, 0},
    {"matrix", required_argument, nullptr, 0},
    {"auto-resize-window", no_argument, nullptr, 0},
    {"cpu-uv-swap", no_argument, nullptr, 0},
    {"help", no_argument, nullptr, 'h'},
    {0,0,0,0}
  };

  for (;;) {
    int idx = 0;
    int c = getopt_long(argc, argv, "h", longopts, &idx);
    if (c == -1) break;
    if (c == 'h') {
      print_usage(argv[0]);
      return 0;
    }
    if (c == 0) {
      std::string name = longopts[idx].name;
      if (name == "uv-swap") {
        std::string v = optarg ? optarg : "auto";
        if (v == "auto") opt_uv_swap_override = -1;
        else if (v == "0") opt_uv_swap_override = 0;
        else if (v == "1") opt_uv_swap_override = 1;
        else { std::cerr << "Invalid uv-swap value\n"; print_usage(argv[0]); return 1; }
      } else if (name == "range") {
        std::string v = optarg ? optarg : "limited";
        if (v == "limited") opt_full_range = 0;
        else if (v == "full") opt_full_range = 1;
        else { std::cerr << "Invalid range\n"; print_usage(argv[0]); return 1; }
      } else if (name == "matrix") {
        std::string v = optarg ? optarg : "709";
        if (v == "709") opt_use_bt709 = 1;
        else if (v == "601") opt_use_bt709 = 0;
        else { std::cerr << "Invalid matrix\n"; print_usage(argv[0]); return 1; }
      } else if (name == "auto-resize-window") {
        opt_auto_resize_window = true;
      } else if (name == "cpu-uv-swap") {
        opt_cpu_uv_swap = true;
      }
    }
  }

  int fd = open(DEVICE, O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    perror("open video0");
    return 1;
  }

  uint32_t cur_width = DEFAULT_WIDTH, cur_height = DEFAULT_HEIGHT, cur_pixfmt = 0;
  if (!get_v4l2_format(fd, cur_width, cur_height, cur_pixfmt)) {
    cur_width = DEFAULT_WIDTH;
    cur_height = DEFAULT_HEIGHT;
  }

  v4l2_format fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  fmt.fmt.pix_mp.width = cur_width;
  fmt.fmt.pix_mp.height = cur_height;
  fmt.fmt.pix_mp.pixelformat = v4l2_fourcc('N','V','2','4');
  fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
  fmt.fmt.pix_mp.num_planes = 1;
  if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
    std::cerr << "VIDIOC_S_FMT: " << strerror(errno) << " (" << errno << ")\n";
  }
  get_v4l2_format(fd, cur_width, cur_height, cur_pixfmt);

  v4l2_event_subscription sub;
  memset(&sub, 0, sizeof(sub));
  sub.type = V4L2_EVENT_SOURCE_CHANGE;
  if (ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
    // not fatal
  }

  v4l2_requestbuffers req;
  memset(&req, 0, sizeof(req));
  req.count = BUF_COUNT;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
    perror("VIDIOC_REQBUFS");
    close(fd);
    return 1;
  }

  std::vector<std::vector<PlaneMap>> buffers(req.count);

  for (unsigned i = 0; i < req.count; ++i) {
    v4l2_buffer buf;
    v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.index = i;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = planes;
    buf.length = VIDEO_MAX_PLANES;

    if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
      perror("VIDIOC_QUERYBUF");
      close(fd);
      return 1;
    }
    buffers[i].resize(buf.length);
    for (unsigned p = 0; p < buf.length; ++p) {
      buffers[i][p].length = planes[p].length;
      buffers[i][p].addr = mmap(nullptr, planes[p].length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, planes[p].m.mem_offset);
      if (buffers[i][p].addr == MAP_FAILED) {
        perror("mmap plane");
        close(fd);
        return 1;
      }
    }
    if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
      perror("VIDIOC_QBUF");
      close(fd);
      return 1;
    }
  }

  int buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (xioctl(fd, VIDIOC_STREAMON, &buf_type) < 0) {
    perror("VIDIOC_STREAMON");
    close(fd);
    return 1;
  }

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
    close(fd);
    return 1;
  }

  SDL_Window* win = SDL_CreateWindow(WINDOW_TITLE,
      SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
      (int)cur_width, (int)cur_height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
  if (!win) {
    std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
    close(fd);
    return 1;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  SDL_GLContext glc = SDL_GL_CreateContext(win);
  if (!glc) {
    std::cerr << "SDL_GL_CreateContext failed: " << SDL_GetError() << std::endl;
    close(fd);
    return 1;
  }

  if (glewInit() != GLEW_OK) {
    std::cerr << "GLEW init failed!" << std::endl;
    close(fd);
    return 1;
  }

  GLint gl_max_tex = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gl_max_tex);

  if (SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) {
    // ignore failure
  }
  bool is_fullscreen = (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;

  int win_w = 0, win_h = 0;
  SDL_GetWindowSize(win, &win_w, &win_h);
  glViewport(0, 0, win_w, win_h);

  {
    std::vector<std::string> attempts;
    std::string vertPath = findShaderFile("shader.vert.glsl", &attempts);
    if (vertPath.empty()) {
      std::cerr << "Vertex shader not found. Tried the following paths:\n";
      for (const auto &p : attempts) std::cerr << "  " << p << "\n";
      close(fd);
      return 1;
    }

    attempts.clear();
    std::string fragPath = findShaderFile("shader.frag.glsl", &attempts);
    if (fragPath.empty()) {
      std::cerr << "Fragment shader not found. Tried the following paths:\n";
      for (const auto &p : attempts) std::cerr << "  " << p << "\n";
      close(fd);
      return 1;
    }

    GLuint program = createShaderProgram(vertPath.c_str(), fragPath.c_str());
    glUseProgram(program);

    float verts[] = {
      -1, -1,     0, 0,
       1, -1,     1, 0,
      -1,  1,     0, 1,
       1,  1,     1, 1,
    };
    GLuint vbo = 0, vao = 0;
    glGenBuffers(1, &vbo);
    glGenVertexArrays(1, &vao);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0); // position
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1); // texcoord
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    GLuint texY = 0, texUV = 0;
    glGenTextures(1, &texY);
    glBindTexture(GL_TEXTURE_2D, texY);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &texUV);
    glBindTexture(GL_TEXTURE_2D, texUV);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    int uv_w = (cur_pixfmt == V4L2_PIX_FMT_NV12 || cur_pixfmt == V4L2_PIX_FMT_NV21) ? (int)(cur_width/2) : (int)cur_width;
    int uv_h = (cur_pixfmt == V4L2_PIX_FMT_NV12 || cur_pixfmt == V4L2_PIX_FMT_NV21) ? (int)(cur_height/2) : (int)cur_height;
    reallocate_textures(texY, texUV, (int)cur_width, (int)cur_height, uv_w, uv_h);

    glUseProgram(program);
    GLint loc_texY = glGetUniformLocation(program, "texY");
    GLint loc_texUV = glGetUniformLocation(program, "texUV");
    if (loc_texY >= 0) glUniform1i(loc_texY, 0);
    if (loc_texUV >= 0) glUniform1i(loc_texUV, 1);

    GLint loc_uv_swap = glGetUniformLocation(program, "uv_swap");
    GLint loc_use_bt709 = glGetUniformLocation(program, "use_bt709");
    GLint loc_full_range = glGetUniformLocation(program, "full_range");
    GLint loc_view_mode = glGetUniformLocation(program, "view_mode");

    // NEW uniform locations
    GLint loc_offsetxy1 = glGetUniformLocation(program, "offsetxy1");
    if (loc_offsetxy1 < 0) {
        std::cerr << "Warning: uniform 'offsetxy1' not found (shader may optimize it out if unused)\n";
    }
    GLint loc_segmentIndex = glGetUniformLocation(program, "segmentIndex"); // may be -1 if shader uses const

    GLint loc_rot = glGetUniformLocation(program, "rot");
    GLint loc_flip_x = glGetUniformLocation(program, "flip_x");
    GLint loc_flip_y = glGetUniformLocation(program, "flip_y");
    // Gap uniforms
    GLint loc_gap_count = glGetUniformLocation(program, "gap_count");
    GLint loc_gap_rows = glGetUniformLocation(program, "gap_rows");

    // Log uniform locations to help debugging
    std::cerr << "Uniform locations: uv_swap=" << loc_uv_swap
              << " rot=" << loc_rot << " flip_x=" << loc_flip_x << " flip_y=" << loc_flip_y
              << " gap_count=" << loc_gap_count << " gap_rows=" << loc_gap_rows
              << " offsetxy1=" << loc_offsetxy1 << " segmentIndex=" << loc_segmentIndex << "\n";

    // Stable defaults for typical HDMI capture
    int uv_swap = 0;
    if (opt_uv_swap_override >= 0) {
      uv_swap = opt_uv_swap_override;
    } else {
      if (cur_pixfmt == V4L2_PIX_FMT_NV21) uv_swap = 1;
      else if (cur_pixfmt == V4L2_PIX_FMT_NV12) uv_swap = 0;
      else uv_swap = 0;
    }
    if (opt_cpu_uv_swap) uv_swap = 0;

    if (loc_uv_swap >= 0) glUniform1i(loc_uv_swap, uv_swap);
    if (loc_use_bt709 >= 0) glUniform1i(loc_use_bt709, opt_use_bt709);
    if (loc_full_range >= 0) glUniform1i(loc_full_range, opt_full_range);
    if (loc_view_mode >= 0) glUniform1i(loc_view_mode, 0);

    // Module files and initial load
    std::array<std::string,3> modFiles = { "modul1.txt", "modul2.txt", "modul3.txt" };
    std::vector<GLint> offsetData;
    if (loadOffsetsFromModuleFiles(modFiles, offsetData)) {
        if (loc_offsetxy1 >= 0 && offsetData.size() >= 150 * 2) {
            glUseProgram(program);
            glUniform2iv(loc_offsetxy1, 150, offsetData.data());
            std::cerr << "Loaded offsetxy1 from module files (initial)\n";
            std::cerr << "offsetData[0..9]:";
            for (size_t i = 0; i < std::min<size_t>(offsetData.size(), 10); ++i) std::cerr << " " << offsetData[i];
            std::cerr << "\n";
        } else {
            std::cerr << "offsetxy1 not uploaded: location invalid or data missing\n";
        }
    } else {
        std::cerr << "Warning: failed to load offset module files on startup\n";
    }

    // Flip/rotation defaults (adjust if needed)
    int flip_x = 1; // horizontal mirror default
    int flip_y = 1; // vertical flip default
    int rotation = 0; // 0..3 (0=0deg,1=90cw,2=180deg,3=270deg)
    // Upload initial values (only if locations valid)
    if (loc_flip_x >= 0) { glUseProgram(program); glUniform1i(loc_flip_x, flip_x); }
    if (loc_flip_y >= 0) { glUseProgram(program); glUniform1i(loc_flip_y, flip_y); }
    if (loc_rot >= 0)    { glUseProgram(program); glUniform1i(loc_rot, rotation); }

    // GAP defaults: default to zero-space after row 5 and after row 10
    const int GAP_ARRAY_SIZE = 8;
    int gap_count = 2;
    int gap_rows_arr[GAP_ARRAY_SIZE] = { 5, 10, 0, 0, 0, 0, 0, 0 };
    if (loc_gap_count >= 0) { glUseProgram(program); glUniform1i(loc_gap_count, gap_count); }
    if (loc_gap_rows >= 0)  { glUseProgram(program); glUniform1iv(loc_gap_rows, GAP_ARRAY_SIZE, gap_rows_arr); }

    bool running = true;
    const uint64_t CHECK_FMT_INTERVAL = 120;
    uint64_t frame_count = 0;
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN | POLLPRI;

    std::vector<unsigned char> tmpUVbuf;
    std::vector<unsigned char> tmpFallback;

    while (running) {
      int ret = poll(&pfd, 1, 2000);
      if (ret < 0) {
        if (errno == EINTR) continue;
        perror("poll");
        break;
      } else if (ret == 0) {
        // timeout
      } else {
        if (pfd.revents & POLLPRI) {
          v4l2_event ev;
          while (ioctl(fd, VIDIOC_DQEVENT, &ev) == 0) {
            if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
              uint32_t new_w=0, new_h=0, new_pf=0;
              if (get_v4l2_format(fd, new_w, new_h, new_pf)) {
                if (new_w != cur_width || new_h != cur_height || new_pf != cur_pixfmt) {
                  cur_width = new_w;
                  cur_height = new_h;
                  cur_pixfmt = new_pf;
                  uv_w = (cur_pixfmt == V4L2_PIX_FMT_NV12 || cur_pixfmt == V4L2_PIX_FMT_NV21) ? (int)(cur_width/2) : (int)cur_width;
                  uv_h = (cur_pixfmt == V4L2_PIX_FMT_NV12 || cur_pixfmt == V4L2_PIX_FMT_NV21) ? (int)(cur_height/2) : (int)cur_height;
                  reallocate_textures(texY, texUV, (int)cur_width, (int)cur_height, uv_w, uv_h);
                  if (opt_auto_resize_window) SDL_SetWindowSize(win, (int)cur_width, (int)cur_height);
                  if (opt_uv_swap_override < 0 && !opt_cpu_uv_swap) {
                    int old_uv = uv_swap;
                    if (cur_pixfmt == V4L2_PIX_FMT_NV21) uv_swap = 1;
                    else if (cur_pixfmt == V4L2_PIX_FMT_NV12) uv_swap = 0;
                    if (uv_swap != old_uv && loc_uv_swap >= 0) {
                      glUseProgram(program);
                      glUniform1i(loc_uv_swap, uv_swap);
                    }
                  }
                }
              }
            }
          }
        }
      }

      if ((frame_count % CHECK_FMT_INTERVAL) == 0) {
        uint32_t new_w=0, new_h=0, new_pf=0;
        if (get_v4l2_format(fd, new_w, new_h, new_pf)) {
          if (new_w != cur_width || new_h != cur_height || new_pf != cur_pixfmt) {
            cur_width = new_w;
            cur_height = new_h;
            cur_pixfmt = new_pf;
            uv_w = (cur_pixfmt == V4L2_PIX_FMT_NV12 || cur_pixfmt == V4L2_PIX_FMT_NV21) ? (int)(cur_width/2) : (int)cur_width;
            uv_h = (cur_pixfmt == V4L2_PIX_FMT_NV12 || cur_pixfmt == V4L2_PIX_FMT_NV21) ? (int)(cur_height/2) : (int)cur_height;
            reallocate_textures(texY, texUV, (int)cur_width, (int)cur_height, uv_w, uv_h);
            if (opt_auto_resize_window) SDL_SetWindowSize(win, (int)cur_width, (int)cur_height);
            if (opt_uv_swap_override < 0 && !opt_cpu_uv_swap) {
              int old_uv = uv_swap;
              if (cur_pixfmt == V4L2_PIX_FMT_NV21) uv_swap = 1;
              else if (cur_pixfmt == V4L2_PIX_FMT_NV12) uv_swap = 0;
              if (uv_swap != old_uv && loc_uv_swap >= 0) {
                glUseProgram(program);
                glUniform1i(loc_uv_swap, uv_swap);
              }
            }
          }
        }
      }

      v4l2_buffer buf;
      v4l2_plane planes[VIDEO_MAX_PLANES];
      memset(&buf, 0, sizeof(buf));
      memset(planes, 0, sizeof(planes));
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.m.planes = planes;
      buf.length = VIDEO_MAX_PLANES;

      if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          ++frame_count;
          continue;
        } else {
          perror("VIDIOC_DQBUF");
          break;
        }
      }

      unsigned char* base = (unsigned char*)buffers[buf.index][0].addr;
      size_t bytesused0 = planes[0].bytesused;

      size_t Y_len = (size_t)cur_width * (size_t)cur_height;
      size_t UV_len = 0;
      bool isNV12_NV21 = (cur_pixfmt == V4L2_PIX_FMT_NV12 || cur_pixfmt == V4L2_PIX_FMT_NV21);
      if (isNV12_NV21) UV_len = (size_t)cur_width * ((size_t)cur_height / 2);
      else UV_len = (size_t)cur_width * (size_t)cur_height * 2;
      size_t total_expected = Y_len + UV_len;

      unsigned char* ybase = nullptr;
      unsigned char* uvbase = nullptr;

      if (buf.length >= 2 && buffers[buf.index].size() >= 2) {
        ybase = (unsigned char*)buffers[buf.index][0].addr;
        uvbase = (unsigned char*)buffers[buf.index][1].addr;
      } else if (bytesused0 >= total_expected) {
        ybase = base;
        uvbase = base + Y_len;
      } else {
        ybase = base;
        uvbase = nullptr;
      }

      if (ybase) {
        if ((int)cur_width <= gl_max_tex && (int)cur_height <= gl_max_tex) {
          glActiveTexture(GL_TEXTURE0);
          glBindTexture(GL_TEXTURE_2D, texY);
          glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
          glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (int)cur_width, (int)cur_height, GL_RED, GL_UNSIGNED_BYTE, ybase);
        } else {
          upload_texture_tiled(GL_RED, texY, (int)cur_width, (int)cur_height, ybase, gl_max_tex, 1);
        }
      }

      if (uvbase) {
        int upload_w = isNV12_NV21 ? (int)(cur_width/2) : (int)cur_width;
        int upload_h = isNV12_NV21 ? (int)(cur_height/2) : (int)cur_height;

        if (opt_cpu_uv_swap && cur_pixfmt == V4L2_PIX_FMT_NV21) {
          size_t need = (size_t)upload_w * (size_t)upload_h * 2;
          if (tmpUVbuf.size() < need) tmpUVbuf.resize(need);
          unsigned char* dst = tmpUVbuf.data();

          if (isNV12_NV21) {
            for (int y = 0; y < upload_h; ++y) {
              const unsigned char* srcRow = uvbase + (size_t)y * (size_t)cur_width;
              unsigned char* dstRow = dst + (size_t)y * (size_t)upload_w * 2;
              for (int x = 0; x < upload_w; ++x) {
                unsigned char v = srcRow[x*2 + 0];
                unsigned char u = srcRow[x*2 + 1];
                dstRow[x*2 + 0] = u;
                dstRow[x*2 + 1] = v;
              }
            }
          } else {
            for (int y = 0; y < upload_h; ++y) {
              const unsigned char* srcRow = uvbase + (size_t)y * (size_t)upload_w * 2;
              unsigned char* dstRow = dst + (size_t)y * (size_t)upload_w * 2;
              for (int x = 0; x < upload_w; ++x) {
                unsigned char v = srcRow[x*2 + 0];
                unsigned char u = srcRow[x*2 + 1];
                dstRow[x*2 + 0] = u;
                dstRow[x*2 + 1] = v;
              }
            }
          }

          if (upload_w <= gl_max_tex && upload_h <= gl_max_tex) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, texUV);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, upload_w, upload_h, GL_RG, GL_UNSIGNED_BYTE, tmpUVbuf.data());
          } else {
            upload_texture_tiled(GL_RG, texUV, upload_w, upload_h, tmpUVbuf.data(), gl_max_tex, 2);
          }
        } else {
          if (upload_w <= gl_max_tex && upload_h <= gl_max_tex) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, texUV);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, upload_w, upload_h, GL_RG, GL_UNSIGNED_BYTE, uvbase);
          } else {
            upload_texture_tiled(GL_RG, texUV, upload_w, upload_h, uvbase, gl_max_tex, 2);
          }
        }
      } else {
        size_t need = Y_len + (size_t)cur_width * (size_t)cur_height * 2;
        if (tmpFallback.size() < need) tmpFallback.resize(need);
        unsigned char* dst = tmpFallback.data();
        unsigned char* src = base;
        for (size_t i = 0, j = 0; i < (size_t)cur_width * (size_t)cur_height; ++i) {
          dst[j++] = src[i*3 + 0];
          dst[j++] = src[i*3 + 1];
          dst[j++] = src[i*3 + 2];
        }
        unsigned char* tmpY = dst;
        unsigned char* tmpUV = dst + Y_len;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texY);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (int)cur_width, (int)cur_height, GL_RED, GL_UNSIGNED_BYTE, tmpY);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, texUV);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (int)cur_width, (int)cur_height, GL_RG, GL_UNSIGNED_BYTE, tmpUV);
      }

      // Draw
      glClear(GL_COLOR_BUFFER_BIT);
      glUseProgram(program);

      // Ensure rotation/flip uniforms are uploaded every frame (avoids optimization/cache issues)
      if (loc_rot >= 0)    glUniform1i(loc_rot, rotation);
      if (loc_flip_x >= 0) glUniform1i(loc_flip_x, flip_x);
      if (loc_flip_y >= 0) glUniform1i(loc_flip_y, flip_y);

      // Ensure gap uniforms are uploaded every frame
      if (loc_gap_count >= 0) glUniform1i(loc_gap_count, gap_count);
      if (loc_gap_rows >= 0)  glUniform1iv(loc_gap_rows, GAP_ARRAY_SIZE, gap_rows_arr);

      if (!opt_cpu_uv_swap && loc_uv_swap >= 0) glUniform1i(loc_uv_swap, uv_swap);
      if (loc_use_bt709 >= 0) glUniform1i(loc_use_bt709, opt_use_bt709);
      if (loc_full_range >= 0) glUniform1i(loc_full_range, opt_full_range);

      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, texY);
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, texUV);

      glBindVertexArray(vao);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

      SDL_GL_SwapWindow(win);

      if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        perror("VIDIOC_QBUF (requeue)");
        break;
      }

      SDL_Event e;
      while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) running = false;
        else if (e.type == SDL_KEYDOWN) {
          SDL_Keycode k = e.key.keysym.sym;
          if (k == SDLK_ESCAPE) running = false;
          else if (k == SDLK_f) {
            if (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) {
              SDL_SetWindowFullscreen(win, 0);
            } else {
              SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
            }
            SDL_GetWindowSize(win, &win_w, &win_h);
            glViewport(0, 0, win_w, win_h);
          } else if (k == SDLK_k) {
            std::vector<GLint> newOffsets;
            if (loadOffsetsFromModuleFiles(modFiles, newOffsets)) {
                std::cerr << "Reload: offsetData[0..9]:";
                for (size_t i = 0; i < std::min<size_t>(newOffsets.size(), 10); ++i) std::cerr << " " << newOffsets[i];
                std::cerr << "\n";
                if (loc_offsetxy1 >= 0 && newOffsets.size() >= 150*2) {
                    glUseProgram(program);
                    glUniform2iv(loc_offsetxy1, 150, newOffsets.data());
                    std::cerr << "Reloaded offsetxy1 from module files (on 'k' press) and uploaded to shader\n";
                    offsetData.swap(newOffsets);
                } else {
                    std::cerr << "Reload failed: loc_offsetxy1 invalid or data incomplete\n";
                }
            } else {
                std::cerr << "Failed to read module files on reload\n";
            }
          } else if (k == SDLK_h) {
            flip_x = !flip_x;
            if (loc_flip_x >= 0) {
                glUseProgram(program);
                glUniform1i(loc_flip_x, flip_x);
            }
            std::cerr << "flip_x = " << flip_x << "\n";
          } else if (k == SDLK_v) {
            flip_y = !flip_y;
            if (loc_flip_y >= 0) {
                glUseProgram(program);
                glUniform1i(loc_flip_y, flip_y);
            }
            std::cerr << "flip_y = " << flip_y << "\n";
          } else if (k == SDLK_r) {
            rotation = (rotation + 2) & 3; // step 180°
            if (loc_rot >= 0) {
                glUseProgram(program);
                glUniform1i(loc_rot, rotation);
            }
            std::cerr << "rotation = " << rotation << " (0=0deg,1=90deg,2=180deg,3=270deg) (step=180°)\n";
          }
        }
      }

      ++frame_count;
    }

    // Cleanup
    glDeleteTextures(1, &texY);
    glDeleteTextures(1, &texUV);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
    SDL_GL_DeleteContext(glc);
    SDL_DestroyWindow(win);
    SDL_Quit();

    for (auto& bufv : buffers)
      for (auto& pm : bufv)
        if (pm.addr && pm.length) munmap(pm.addr, pm.length);

    close(fd);
    return 0;
  }
}
