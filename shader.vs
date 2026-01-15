attribute vec2 aPos;
attribute vec2 aTex;

varying vec2 vTexCoord;

uniform mat4 uTransform;

void main() {
    gl_Position = uTransform * vec4(aPos, 0.0, 1.0);
    vTexCoord = aTex;
}
