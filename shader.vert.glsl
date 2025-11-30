#version 140

in vec2 position;   // attribute 0
in vec2 texcoord;   // attribute 1

out vec2 TexCoord;

void main() {
    TexCoord = vec2(texcoord.x, 1.0 - texcoord.y); // flip vertical
    gl_Position = vec4(position, 0.0, 1.0);
}