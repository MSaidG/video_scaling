#extension GL_OES_EGL_image_external : require
precision mediump float;

varying vec2 vTexCoord;
uniform samplerExternalOES uTexture; // External sampler handles YUV->RGB

void main() {
    // The hardware handles the conversion; we just sample it like a normal RGB texture
    gl_FragColor = texture2D(uTexture, vTexCoord);
}




