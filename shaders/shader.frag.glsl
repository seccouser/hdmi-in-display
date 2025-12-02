#version 300 es
precision mediump float;

// Input texture coordinate from vertex shader
in vec2 v_texCoord;

// Output fragment color
out vec4 fragColor;

// Camera/video frame texture
uniform sampler2D texFrame;

// Test pattern texture
uniform sampler2D texPattern;

// Control whether to show test pattern (1) or camera frame (0)
uniform int u_showPattern;

void main() {
    if (u_showPattern == 1) {
        fragColor = texture(texPattern, v_texCoord);
    } else {
        fragColor = texture(texFrame, v_texCoord);
    }
}
