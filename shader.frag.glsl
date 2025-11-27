#version 140

in vec2 TexCoord; // Wird hier nicht direkt verwendet, Mapping übernimmt die Logik!
out vec4 FragColor;

// --- YUV-Textures ---
uniform sampler2D texY;   // Y-Komponente
uniform sampler2D texUV;  // UV-Komponente

// --- Mosaik-/Segment-Uniforms ---
uniform int segmentIndex; // 1-16: Quellbereich (wird vom Programm gesetzt)
const vec2 fullInputSize = vec2(3840.0, 2160.0);
const int segmentsX = 3;
const int segmentsY = 3;
const vec2 subBlockSize = vec2(1280.0, 720.0);

// --- Kachelgrößen/Abstände im Ausgangsbild ---
const float tileW = 128.0;
const float tileH = 144.0;
const float spacingX = 98.0;
const float spacingY = 90.0;
const float marginX = 0.0;
const int numTilesPerRow = 10;
const int numTilesPerCol = 15;

// --- Offsets: werden aus C++ per glUniform2iv() gefüllt ---
// offsetxy1[0..149] = ivec2 pairs for modules 1..3 concatenated (50 each)
// Interpretation: offsetxy1[i].x positive = move tile to the right; negative = left
//                 offsetxy1[i].y positive = move tile down; negative = up
uniform ivec2 offsetxy1[150];

// --- Rotation/Flip controls ---
uniform int rot;      // 0=0deg,1=90degcw,2=180deg,3=270degcw
uniform int flip_x;   // 0 = normal, 1 = mirrored horizontally (input texture)
uniform int flip_y;   // 0 = normal, 1 = mirrored vertically (input texture)

// --- YUV-Parameter ---
uniform int uv_swap;      // 0 = U in .r, V in .g ; 1 = swapped
uniform int full_range;   // 0 = limited (video), 1 = full (pc)
uniform int use_bt709;    // 1 = BT.709, 0 = BT.601
uniform int view_mode;    // 0 = normal, 1 = show Y, 2 = show U, 3 = show V

// helper: rotate a point (u,v) around center (0.5,0.5) by k*90deg clockwise
vec2 rotate90_centered(vec2 uv, int k) {
    vec2 c = vec2(0.5, 0.5);
    vec2 p = uv - c;
    vec2 r;
    int kk = k & 3;
    if (kk == 0) {
        r = p;
    } else if (kk == 1) {
        // 90 cw: (x,y) -> (y, -x)
        r = vec2(p.y, -p.x);
    } else if (kk == 2) {
        // 180: (x,y) -> (-x, -y)
        r = vec2(-p.x, -p.y);
    } else {
        // 270 cw: (x,y) -> (-y, x)
        r = vec2(-p.y, p.x);
    }
    return r + c;
}

// --- Mapping: OpenGL 3.1+; gl_FragCoord.xy integer Pixelposition im Zielbild! ---
void main()
{
    vec2 outPx = gl_FragCoord.xy;

    // --- Subblock berechnen ---
    int segIdx = clamp(segmentIndex, 1, 16) - 1;
    int segCol = segIdx % segmentsX;
    int segRow = segIdx / segmentsX;
    vec2 subBlockOrigin = vec2(float(segCol) * subBlockSize.x, float(segRow) * subBlockSize.y);

    // --- Kachelbereich berechnen (base grid position) ---
    int tileCol = int((outPx.x - marginX) / (tileW + spacingX));
    int tileRow = int(outPx.y / (tileH + spacingY));
    // If outPx is outside the regular grid bounds, early out
    if (tileCol < 0 || tileCol >= numTilesPerRow || tileRow < 0 || tileRow >= numTilesPerCol) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float tileStartX = marginX + float(tileCol) * (tileW + spacingX);
    float tileStartY = float(tileRow) * (tileH + spacingY);

    // Determine index into offsets array (row-major within the subblock)
    int tileIndexWithinSubblock = tileRow * numTilesPerRow + tileCol; // 0 .. (numTilesPerRow*numTilesPerCol-1)
    int globalIndex = clamp(tileIndexWithinSubblock, 0, 149);
    ivec2 off = offsetxy1[globalIndex];

    // --- Apply offset to tile's displayed rectangle (move the tile on the output) ---
    // Interpret off.x (GLint): positive -> shift right, negative -> left
    //              off.y (GLint): positive -> shift down, negative -> up
    vec2 tileRectStart = vec2(tileStartX, tileStartY) + vec2(float(off.x), float(off.y));
    vec2 tileRectEnd = tileRectStart + vec2(tileW, tileH);

    // --- Check if current pixel lies in the (offset) tile rectangle ---
    bool inTile = (outPx.x >= tileRectStart.x && outPx.x < tileRectEnd.x &&
                   outPx.y >= tileRectStart.y && outPx.y < tileRectEnd.y);

    if (!inTile) {
        // not in this tile's displayed rectangle -> black (gap)
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // --- Offset within the displayed tile (pixel coords, 0..tileW-1 / 0..tileH-1) ---
    float pxInTileX = outPx.x - tileRectStart.x; // 0 .. tileW-1
    float pxInTileY = outPx.y - tileRectStart.y; // 0 .. tileH-1

    // --- Compute source position in the input image that corresponds to this tile pixel ---
    // We keep the original mapping from tileCol/tileRow to source subblock:
    // fetchX/fetchY compute the input pixel coordinate in the full input that this tile pixel should sample.
    float fetchX = tileW * float(tileCol) + pxInTileX;
    float fetchY = tileH * float(tileRow) + pxInTileY;
    vec2 inputCoord = subBlockOrigin + vec2(fetchX, fetchY);

    // Note: We intentionally DO NOT apply offsetxy1 to inputCoord here.
    // The offsets only move the tile in the OUTPUT (destination), as requested.

    // clamp inputCoord into valid input range (avoid sampling outside)
    inputCoord = clamp(inputCoord, vec2(0.0), fullInputSize - vec2(1.0));

    // --- Texturkoordinaten auf [0,1] ---
    vec2 inputUVCoord = inputCoord / fullInputSize;

    // --- APPLY ROTATION / FLIP to the INPUT UV before sampling ---
    // rotate around full input center, then flip, then clamp to [0,1]
    vec2 uvTrans = inputUVCoord;
    uvTrans = rotate90_centered(uvTrans, rot);
    if (flip_x == 1) uvTrans.x = 1.0 - uvTrans.x;
    if (flip_y == 1) uvTrans.y = 1.0 - uvTrans.y;
    uvTrans = clamp(uvTrans, vec2(0.0), vec2(1.0));

    // --- YUV Sample using transformed input UV ---
    float Y = texture(texY, uvTrans).r * 255.0;
    vec2 uv = texture(texUV, uvTrans).rg * 255.0;

    float U = uv.x;
    float V = uv.y;
    if (uv_swap == 1) { float tmp = U; U = V; V = tmp; }

    // --- Debugkanal-View ---
    if (view_mode == 1) {
        float yy = clamp(Y / 255.0, 0.0, 1.0);
        FragColor = vec4(vec3(yy), 1.0);
        return;
    } else if (view_mode == 2) {
        float uu = clamp((U - 128.0) / 256.0 + 0.5, 0.0, 1.0);
        FragColor = vec4(vec3(uu), 1.0);
        return;
    } else if (view_mode == 3) {
        float vv = clamp((V - 128.0) / 256.0 + 0.5, 0.0, 1.0);
        FragColor = vec4(vec3(vv), 1.0);
        return;
    }

    // --- YUV→RGB Umwandlung ---
    float y;
    if (full_range == 1) y = Y;
    else y = 1.164383 * (Y - 16.0);

    float u = U - 128.0;
    float v = V - 128.0;

    vec3 rgb;
    if (use_bt709 == 1) {
        float r = y + 1.792741 * v;
        float g = y - 0.213249 * u - 0.532909 * v;
        float b = y + 2.112402 * u;
        rgb = vec3(r, g, b);
    } else {
        float r = y + 1.596027 * v;
        float g = y - 0.391762 * u - 0.812968 * v;
        float b = y + 2.017232 * u;
        rgb = vec3(r, g, b);
    }

    rgb = clamp(rgb / 255.0, vec3(0.0), vec3(1.0));
    FragColor = vec4(rgb, 1.0);
}
