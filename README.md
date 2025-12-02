# HDMI Simple Display

A simple application that captures HDMI input via V4L2 and displays it using OpenGL ES. Includes test pattern fallback when no signal is available.

## Features

- V4L2 video capture with robust error handling
- Automatic test pattern fallback when no signal is detected
- Support for user-supplied test pattern images (JPEG, PNG, etc.)
- Procedural color bars pattern as fallback

## Building

```bash
mkdir build && cd build
cmake ..
make
```

## Usage

```bash
./hdmi_simple_display [options]

Options:
  -d, --device <path>       V4L2 device path (default: /dev/video0)
  -t, --test-pattern <path> Path to test pattern image
  -v, --verbose             Enable verbose logging
  -h, --help                Show help message
```

## Test Pattern Search

When `--test-pattern` is not specified, the application searches for a test image in these locations (in order):

1. `testimage.jpg`
2. `test_image.jpg`
3. `testpattern.png`
4. `resources/testimage.jpg`
5. `assets/testimage.png`
6. Executable directory and its `shaders/` and `assets/` subdirectories

If no image is found, a procedural color bars pattern is generated.

## Dependencies

- V4L2 (Video for Linux 2)
- OpenGL ES 3.0 / EGL (optional)
- stb_image.h (included in repository)

## License

See `resources/LICENSE` for details.