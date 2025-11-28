#version 140

in vec2 TexCoord;
out vec4 FragColor;

uniform sampler2D texY;
uniform sampler2D texUV;

uniform int segmentIndex;
const vec2 fullInputSize = vec2(3840.0, 2160.0);
const int segmentsX = 3;
const int segmentsY = 3;
const vec2 subBlockSize = vec2(1280.0, 720.0);

const float tileW = 128.0;
const float tileH = 144.0;
const float spacingX = 98.0;
const float spacingY = 90.0;
const float marginX = 0.0;
const int numTilesPerRow = 10;
const int numTilesPerCol = 15;

uniform ivec2 offsetxy1[150];

uniform int rot;
uniform int flip_x;
uniform int flip_y;

uniform int gap_count;
uniform int gap_rows[8];

// NEW: control whether input tiles are ordered top->down (1) or bottom->up (0)
uniform int inputTilesTopToBottom;

uniform int uv_swap;
uniform int full_range;
uniform int use_bt709;
uniform int view_mode;

vec2 rotate90_centered(vec2 uv, int k) {
    vec2 c = vec2(0.5, 0.5);
    vec2 p = uv - c;
    vec2 r;
    int kk = k & 3;
    if (kk == 0) r = p;
    else if (kk == 1) r = vec2(p.y, -p.x);
    else if (kk == 2) r = vec2(-p.x, -p.y);
    else r = vec2(-p.y, p.x);
    return r + c;
}

bool isGapZero(int gapIdx) {
    for (int i = 0; i < 8; ++i) {
        if (i >= gap_count) break;
        if (gap_rows[i] == gapIdx) return true;
    }
    return false;
}

float computeTotalGridHeight() {
    float h = 0.0;
    for (int r = 0; r < numTilesPerCol; ++r) {
        h += tileH;
        if (r < numTilesPerCol - 1) {
            if (!isGapZero(r + 1)) h += spacingY;
        }
    }
    return h;
}

void main()
{
    vec2 outPx = gl_FragCoord.xy;

    int segIdx = clamp(segmentIndex, 1, 16) - 1;
    int segCol = segIdx % segmentsX;
    int segRow = segIdx / segmentsX;
    vec2 subBlockOrigin = vec2(float(segCol) * subBlockSize.x, float(segRow) * subBlockSize.y);

    int tileCol = int((outPx.x - marginX) / (tileW + spacingX));

    float totalGridH = computeTotalGridHeight();
    float yFromTop = totalGridH - outPx.y;

    int tileRow = -1;
    float yAccTop = 0.0;
    for (int r = 0; r < numTilesPerCol; ++r) {
        float rowStart = yAccTop;
        float rowEnd = rowStart + tileH;
        if (yFromTop >= rowStart && yFromTop < rowEnd) {
            tileRow = r;
            break;
        }
        bool gapAfterThisRow = isGapZero(r + 1);
        if (!gapAfterThisRow) yAccTop = rowEnd + spacingY;
        else yAccTop = rowEnd;
    }

    if (tileCol < 0 || tileCol >= numTilesPerRow || tileRow < 0 || tileRow >= numTilesPerCol) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float tileStartX = marginX + float(tileCol) * (tileW + spacingX);

    float tileStartY_top = 0.0;
    for (int r = 0; r < tileRow; ++r) {
        tileStartY_top += tileH;
        bool gapAfterThisRow = isGapZero(r + 1);
        if (!gapAfterThisRow) tileStartY_top += spacingY;
    }
    float tileStartY = totalGridH - (tileStartY_top + tileH);

    int tileIndexWithinSubblock = tileRow * numTilesPerRow + tileCol;
    int globalIndex = clamp(tileIndexWithinSubblock, 0, 149);
    ivec2 off = offsetxy1[globalIndex];

    vec2 tileRectStart = vec2(tileStartX, tileStartY) + vec2(float(off.x), float(off.y));
    vec2 tileRectEnd = tileRectStart + vec2(tileW, tileH);

    if (!(outPx.x >= tileRectStart.x && outPx.x < tileRectEnd.x &&
          outPx.y >= tileRectStart.y && outPx.y < tileRectEnd.y)) {
        FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float pxInTileX = outPx.x - tileRectStart.x;
    float pxInTileY = outPx.y - tileRectStart.y;

    // Determine source row depending on inputTilesTopToBottom
    int sourceTileRow;
    if (inputTilesTopToBottom == 1) {
        sourceTileRow = tileRow;
    } else {
        sourceTileRow = (numTilesPerCol - 1) - tileRow;
    }

    // Map to source pixel (do NOT invert pxInTileY here)
    float fetchX = tileW * float(tileCol) + pxInTileX;
    float fetchY = tileH * float(sourceTileRow) + pxInTileY;
    vec2 inputCoord = subBlockOrigin + vec2(fetchX, fetchY);

    inputCoord = clamp(inputCoord, vec2(0.0), fullInputSize - vec2(1.0));

    vec2 inputUVCoord = inputCoord / fullInputSize;

    vec2 uvTrans = rotate90_centered(inputUVCoord, rot);
    if (flip_x == 1) uvTrans.x = 1.0 - uvTrans.x;
    if (flip_y == 1) uvTrans.y = 1.0 - uvTrans.y;
    uvTrans = clamp(uvTrans, vec2(0.0), vec2(1.0));

    float Y = texture(texY, uvTrans).r * 255.0;
    vec2 uv = texture(texUV, uvTrans).rg * 255.0;

    float U = uv.x;
    float V = uv.y;
    if (uv_swap == 1) { float tmp = U; U = V; V = tmp; }

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
