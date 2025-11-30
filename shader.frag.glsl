#version 140

in vec2 TexCoord; // Wird hier nicht direkt verwendet, Mapping übernimmt die Logik!
out vec4 FragColor;

// --- YUV-Textures ---
uniform sampler2D texY;   // Y-Komponente
uniform sampler2D texUV;  // UV-Komponente

// --- Segment-Uniform (wird vom Programm gesetzt) ---
uniform int segmentIndex; // 1..16

// --- Mosaik-/Segment-Uniforms (now configurable) ---
uniform vec2 u_fullInputSize;       // was const vec2 fullInputSize
uniform int  u_segmentsX;           // was const int segmentsX
uniform int  u_segmentsY;           // was const int segmentsY
uniform vec2 u_subBlockSize;        // was const vec2 subBlockSize

// --- Kachelgrößen/Abstände im Ausgangsbild (configurable) ---
uniform float u_tileW;
uniform float u_tileH;
uniform float u_spacingX;
uniform float u_spacingY;
uniform float u_marginX;
uniform int   u_numTilesPerRow;
uniform int   u_numTilesPerCol;

// --- Offsets: werden aus C++ per glUniform2iv() gefüllt ---
uniform ivec2 offsetxy1[150];

// --- Rotation/Flip controls ---
uniform int rot;      // 0=0deg,1=90degcw,2=180deg,3=270degcw
uniform int flip_x;   // 0 = normal, 1 = mirrored horizontally (input texture)
uniform int flip_y;   // 0 = normal, 1 = mirrored vertically (input texture)

// --- Gap controls ---
uniform int gap_count;
uniform int gap_rows[8];

// --- Control: input tile order ---
uniform int inputTilesTopToBottom;

// --- Module serial numbers (for info/diagnostics) ---
uniform int moduleSerials[3];

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

bool isGapZero(int gapIdx) {
    for (int i = 0; i < 8; ++i) {
        if (i >= gap_count) break;
        if (gap_rows[i] == gapIdx) return true;
    }
    return false;
}

// compute total grid height (sum of all rows + spacing considering gaps)
float computeTotalGridHeight(int numRows, float tileH, float spacingY) {
    float h = 0.0;
    for (int r = 0; r < numRows; ++r) {
        h += tileH;
        if (r < numRows - 1) {
            if (!isGapZero(r + 1)) h += spacingY;
        }
    }
    return h;
}

// --- Mapping: OpenGL 3.1+; gl_FragCoord.xy integer Pixelposition im Zielbild! ---
void main()
{
    vec2 outPx = gl_FragCoord.xy;

    // --- Subblock berechnen ---
    int segIdx = clamp(segmentIndex, 1, 16) - 1;
    int segCol = segIdx % u_segmentsX;
    int segRow = segIdx / u_segmentsX;
    vec2 subBlockOrigin = vec2(float(segCol) * u_subBlockSize.x, float(segRow) * u_subBlockSize.y);

    // --- X (tileCol) ---
    int tileCol = int((outPx.x - u_marginX) / (u_tileW + u_spacingX));

    // --- compute total grid height using uniforms ---
    float totalGridH = computeTotalGridHeight(u_numTilesPerCol, u_tileH, u_spacingY);

    // yFromTop: distance in pixels from top of the grid
    float yFromTop = totalGridH - outPx.y;

    // --- tileRow: accumulate rows from top using yFromTop ---
    int tileRow = -1;
    float yAccTop = 0.0;
    for (int r = 0; r < u_numTilesPerCol; ++r) {
        float rowStart = yAccTop;
        float rowEnd = rowStart + u_tileH;
        if (yFromTop >= rowStart && yFromTop < rowEnd) {
            tileRow = r;
            break;
        }
        bool gapAfterThisRow = isGapZero(r + 1);
        if (!gapAfterThisRow) yAccTop = rowEnd + u_spacingY;
        else yAccTop = rowEnd;
    }

    // quick reject
    if (tileCol < 0 || tileCol >= u_numTilesPerRow || tileRow < 0 || tileRow >= u_numTilesPerCol) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // --- Compute tileStartX (left) ---
    float tileStartX = u_marginX + float(tileCol) * (u_tileW + u_spacingX);

    // --- Compute tileStartY measured from TOP, convert to bottom-origin coordinates
    float tileStartY_top = 0.0;
    for (int r = 0; r < tileRow; ++r) {
        tileStartY_top += u_tileH;
        bool gapAfterThisRow = isGapZero(r + 1);
        if (!gapAfterThisRow) tileStartY_top += u_spacingY;
    }
    float tileStartY = totalGridH - (tileStartY_top + u_tileH);

    // Determine index into offset array (row-major within the subblock)
    int tileIndexWithinSubblock = tileRow * u_numTilesPerRow + tileCol;
    int globalIndex = clamp(tileIndexWithinSubblock, 0, 149);
    ivec2 off = offsetxy1[globalIndex];

    // Apply per-tile output offset (pixel units)
    vec2 tileRectStart = vec2(tileStartX, tileStartY) + vec2(float(off.x), float(off.y));
    vec2 tileRectEnd = tileRectStart + vec2(u_tileW, u_tileH);

    // Check whether current pixel lies within the (offset) tile rectangle
    if (!(outPx.x >= tileRectStart.x && outPx.x < tileRectEnd.x &&
          outPx.y >= tileRectStart.y && outPx.y < tileRectEnd.y)) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Pixel inside displayed tile
    float pxInTileX = outPx.x - tileRectStart.x;
    float pxInTileY = outPx.y - tileRectStart.y;

    // Determine source row depending on inputTilesTopToBottom
    int sourceTileRow;
    if (inputTilesTopToBottom == 1) {
        sourceTileRow = tileRow;
    } else {
        sourceTileRow = (u_numTilesPerCol - 1) - tileRow;
    }

    // Map to source pixel (do NOT invert pxInTileY here)
    float fetchX = u_tileW * float(tileCol) + pxInTileX;
    float fetchY = u_tileH * float(sourceTileRow) + pxInTileY;
    vec2 inputCoord = subBlockOrigin + vec2(fetchX, fetchY);

    // clamp inputCoord
    inputCoord = clamp(inputCoord, vec2(0.0), u_fullInputSize - vec2(1.0));

    // --- Texturkoordinaten auf [0,1] ---
    vec2 inputUVCoord = inputCoord / u_fullInputSize;

    // --- APPLY ROTATION / FLIP to the INPUT UV before sampling ---
    vec2 uvTrans = rotate90_centered(inputUVCoord, rot);
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
