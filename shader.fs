#extension GL_OES_EGL_image_external : require
precision mediump float;

varying vec2 vTexCoord;
uniform samplerExternalOES uTexture; // External sampler handles YUV->RGB

void main() {
    // The hardware handles the conversion; we just sample it like a normal RGB texture
    gl_FragColor = texture2D(uTexture, vTexCoord);
}

// precision mediump float;
// varying vec2 vTex;

// uniform sampler2D uTexY;
// uniform sampler2D uTexU;
// uniform sampler2D uTexV;

// void main() {
//     float y = texture2D(uTexY, vTex).r;
//     float u = texture2D(uTexU, vTex).r - 0.5;
//     float v = texture2D(uTexV, vTex).r - 0.5;

//     float r = y + 1.402 * v;
//     float g = y - 0.344136 * u - 0.714136 * v;
//     float b = y + 1.772 * u;

//     gl_FragColor = vec4(r, g, b, 1.0);
// }


