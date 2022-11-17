@module goodpixel

@vs vs
in vec4 coord;
out vec2 texUV;
void main() {
    gl_Position = vec4(coord.xy, 0.0, 1.0);
    texUV = coord.zw;
}
@end

@fs fs
uniform sampler2D iChannel0;
uniform fs_params {
    vec4 iColor;
};
in vec2 texUV;
out vec4 fragColor;

vec4 texture2DAA(sampler2D tex, vec2 uv) {
    vec2 texsize = vec2(textureSize(tex,0));
    vec2 uv_texspace = uv*texsize;
    vec2 seam = floor(uv_texspace+.5);
    uv_texspace = (uv_texspace-seam)/fwidth(uv_texspace)+seam;
    uv_texspace = clamp(uv_texspace, seam-.5, seam+.5);
    return texture(tex, uv_texspace/texsize);
}

void main() {
    fragColor = texture2DAA(iChannel0, texUV) * iColor;
}
@end

@program program vs fs
