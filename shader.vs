attribute vec4 aPos;
attribute vec2 aTex;
varying vec2 vTexCoord;

void main() {
    gl_Position = aPos;
    vTexCoord = aTex;
}
