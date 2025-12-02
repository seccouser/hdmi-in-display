// hdmi_simple_display.cpp - HDMI IN Display Application
// Displays HDMI input on screen using OpenGL

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// Debug mode enumeration
enum class DebugSimpleMode {
    NONE,      // No debug mode (default)
    GL_SWAP,   // Test GL/swap/viewport - renders solid color
    UV_GRID    // Test shader/texture sampling - renders UV gradient
};

// Global debug mode
static DebugSimpleMode g_debugSimpleMode = DebugSimpleMode::NONE;

// CLI option prefix constants
static constexpr const char* DEBUG_SIMPLE_PREFIX = "--debug-simple=";

// Helper to calculate string length at compile time
constexpr size_t constexprStrlen(const char* str) {
    size_t len = 0;
    while (str[len] != '\0') ++len;
    return len;
}
static constexpr size_t DEBUG_SIMPLE_PREFIX_LEN = constexprStrlen(DEBUG_SIMPLE_PREFIX);

// Print usage information
void printUsage(const char* programName) {
    printf("Usage: %s [OPTIONS]\n", programName);
    printf("\n");
    printf("Options:\n");
    printf("  --debug-simple=MODE  Enable debug mode to isolate rendering issues\n");
    printf("                       MODE can be:\n");
    printf("                         gl_swap  - Render solid color to test GL/swap/viewport\n");
    printf("                         uv_grid  - Render UV gradient to test shader/texture sampling\n");
    printf("  --help               Show this help message\n");
    printf("\n");
    printf("Debug Modes:\n");
    printf("  gl_swap:  Renders a solid color (green) to verify that the OpenGL context,\n");
    printf("            swap chain, and viewport are working correctly.\n");
    printf("  uv_grid:  Renders a UV coordinate gradient to verify that shaders and\n");
    printf("            texture sampling are working, bypassing the YUV pipeline.\n");
}

// Parse command line arguments
bool parseArgs(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return false;
        }
        if (strncmp(argv[i], DEBUG_SIMPLE_PREFIX, DEBUG_SIMPLE_PREFIX_LEN) == 0) {
            const char* mode = argv[i] + DEBUG_SIMPLE_PREFIX_LEN;
            if (mode[0] == '\0') {
                fprintf(stderr, "Error: --debug-simple requires a mode value\n");
                fprintf(stderr, "Valid modes are: gl_swap, uv_grid\n");
                return false;
            }
            if (strcmp(mode, "gl_swap") == 0) {
                g_debugSimpleMode = DebugSimpleMode::GL_SWAP;
                printf("[DEBUG] Debug mode enabled: gl_swap (solid color test)\n");
            } else if (strcmp(mode, "uv_grid") == 0) {
                g_debugSimpleMode = DebugSimpleMode::UV_GRID;
                printf("[DEBUG] Debug mode enabled: uv_grid (UV gradient test)\n");
            } else {
                fprintf(stderr, "Error: Unknown debug mode '%s'\n", mode);
                fprintf(stderr, "Valid modes are: gl_swap, uv_grid\n");
                return false;
            }
        } else {
            fprintf(stderr, "Error: Unknown argument '%s'\n", argv[i]);
            fprintf(stderr, "Use --help to see available options.\n");
            return false;
        }
    }
    return true;
}

// Get the current debug mode
DebugSimpleMode getDebugSimpleMode() {
    return g_debugSimpleMode;
}

// Check if any debug mode is active
bool isDebugModeActive() {
    return g_debugSimpleMode != DebugSimpleMode::NONE;
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    if (!parseArgs(argc, argv)) {
        return 1;
    }

    printf("HDMI Simple Display starting...\n");

    // Debug mode handling
    switch (g_debugSimpleMode) {
        case DebugSimpleMode::GL_SWAP:
            printf("Running in GL_SWAP debug mode - will render solid color\n");
            // In a full implementation, this would:
            // - Initialize OpenGL context
            // - Clear to solid green color
            // - Swap buffers
            // - Skip YUV texture upload and shader pipeline
            break;

        case DebugSimpleMode::UV_GRID:
            printf("Running in UV_GRID debug mode - will render UV gradient\n");
            // In a full implementation, this would:
            // - Initialize OpenGL context
            // - Use a simple shader that outputs UV coordinates as colors
            // - Render a fullscreen quad
            // - Skip YUV texture and conversion
            break;

        case DebugSimpleMode::NONE:
        default:
            printf("Running in normal mode - will process HDMI input\n");
            // Normal operation: capture HDMI input and display via YUV pipeline
            break;
    }

    printf("HDMI Simple Display exiting.\n");
    return 0;
}