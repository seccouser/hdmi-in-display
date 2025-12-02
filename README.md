# HDMI Simple Display

A robust HDMI input to display application for Linux embedded systems. Captures HDMI input via V4L2 (e.g., rk_hdmirx driver) and displays it on screen using DRM/KMS.

## Features

- **Robust Input Handling**: Gracefully handles HDMI input interruptions (cable disconnect, source change)
- **Test Pattern Fallback**: Shows a configurable test pattern image when input is lost
- **Automatic Recovery**: Reliably recovers to live video when the HDMI source returns
- **Graceful Shutdown**: Responds to SIGINT/SIGTERM for clean termination

## Building

```bash
mkdir build && cd build
cmake ..
make
```

## Usage

```bash
hdmi_simple_display [OPTIONS]
```

### Options

| Option | Description |
|--------|-------------|
| `--test-pattern=<path>` | Path to test pattern image (PNG/JPEG) to show when HDMI input is lost |
| `--video-device=<path>` | V4L2 video device (default: `/dev/video0`) |
| `--help` | Show help message |

### Examples

Basic usage with default settings:
```bash
./hdmi_simple_display
```

With a custom test pattern:
```bash
./hdmi_simple_display --test-pattern=/usr/share/images/test-pattern.png
```

With a custom video device:
```bash
./hdmi_simple_display --video-device=/dev/video1 --test-pattern=/path/to/pattern.jpg
```

## Systemd Service

A systemd service file is provided in `service/hdmi-display.service`. Install it with:

```bash
sudo cp service/hdmi-display.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable hdmi-display
sudo systemctl start hdmi-display
```

## Technical Details

### Input State Machine

The application operates with the following states:

1. **Unknown**: Initial state, attempting to start capture
2. **Active**: HDMI input is valid, capturing and displaying frames
3. **Lost**: HDMI input was lost (EINVAL/EIO from VIDIOC_DQBUF), showing test pattern

### Error Handling

When the rk_hdmirx driver (or similar) returns errors from `VIDIOC_DQBUF`:

- `EINVAL`: Source format changed or driver error - triggers recovery
- `EIO`: I/O error (cable disconnect) - triggers recovery
- `EAGAIN`: Normal for non-blocking I/O, continues waiting

## License

See [LICENSE](resources/LICENSE) for details.