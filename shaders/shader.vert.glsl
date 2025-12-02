#version 300 es
precision mediump float;

// Vertex position input
in vec2 a_position;

// Texture coordinate input
in vec2 a_texCoord;

// Texture coordinate output to fragment shader
out vec2 v_texCoord;

void main() {
    gl_Position = vec4(a_position, 0.0, 1.0);
    v_texCoord = a_texCoord;
}
