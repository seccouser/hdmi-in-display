# HDMI-IN Display

A simple HDMI input display application.

## Building

```bash
mkdir build && cd build
cmake ..
make
```

## Usage

```bash
./hdmi_simple_display [OPTIONS]
```

### Options

| Option | Description |
|--------|-------------|
| `--help` | Show help message |
| `--debug-simple=MODE` | Enable debug mode (see below) |

### Debug Modes

The `--debug-simple=` flag accepts the following modes:

- **gl_swap**: Renders a solid color (green) to verify that the OpenGL context, swap chain, and viewport are working correctly. Use this to isolate GL/swap/viewport issues.

- **uv_grid**: Renders a UV coordinate gradient to verify that shaders and texture sampling are working, bypassing the YUV pipeline. Use this to isolate shader/texture sampling issues from YUV pipeline issues.

### Examples

```bash
# Normal operation
./hdmi_simple_display

# Test GL/swap/viewport
./hdmi_simple_display --debug-simple=gl_swap

# Test shader/texture sampling
./hdmi_simple_display --debug-simple=uv_grid
```

## Debugging Workflow

When troubleshooting display issues, use the debug modes in this order:

1. **gl_swap**: If this fails, the problem is with GL context, swap chain, or viewport setup.
2. **uv_grid**: If gl_swap works but this fails, the problem is with shader compilation or texture sampling.
3. **Normal mode**: If uv_grid works but normal mode fails, the problem is in the YUV pipeline (texture upload, YUV-to-RGB conversion, or HDMI capture).

## License

See [resources/LICENSE](resources/LICENSE) for license information.