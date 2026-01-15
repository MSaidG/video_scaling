// #extension GL_OES_EGL_image_external : require
// precision mediump float;

// varying vec2 vTexCoord;
// uniform samplerExternalOES uTexture; 

// void main() {
//     // gl_FragColor = texture2D(uTexture, vTexCoord);
//     // vec4 color = texture2D(uTexture, vTexCoord); 
//     // gl_FragColor = vec4(color.r, color.r, color.r, color.a);

//     vec4 color = texture2D(uTexture, vTexCoord);
//     gl_FragColor = color.bgra;
// }




precision mediump float;

varying vec2 vTexCoord;
uniform sampler2D uTextureY;  // Texture Unit 0
uniform sampler2D uTextureUV; // Texture Unit 1

// YUV to RGB Transformation Matrix (ITU-R BT.601)
const mat3 yuv2rgb = mat3(
    1.164,  1.164, 1.164,
    0.000, -0.392, 2.017,
    1.596, -0.813, 0.000
);

void main() {
    // 1. Read Y (Brightness) from the first texture (Red channel)
    float y = texture2D(uTextureY, vTexCoord).r;

    // 2. Read UV (Color) from the second texture (Red & Green channels)
    // Note: In NV12, U and V are interleaved. We read them as a "RG" texture.
    // .r = U, .g = V (or vice versa depending on endianness, usually .ra or .rg)
    vec2 uv = texture2D(uTextureUV, vTexCoord).rg - 0.5; 
    
    // 3. Convert to RGB
    vec3 rgb = yuv2rgb * vec3(y, uv.x, uv.y);

    gl_FragColor = vec4(rgb, 1.0);
}
