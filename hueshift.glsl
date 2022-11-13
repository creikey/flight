@module hueshift

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
uniform uniforms {
    int is_colorless; // if greater than zero, no color
    float target_hue;
};
in vec2 texUV;
out vec4 fragColor;

vec3 rgb2hsv(vec3 c)
{
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));

    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

vec3 hsv2rgb(vec3 c)
{
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main() {
    vec4 outColor = texture(iChannel0, texUV);
    vec3 hsv = rgb2hsv(outColor.rgb);
    
    if(is_colorless > 0) 
    {
        hsv.y = 0.0f;
    } else if(hsv.y > 0.5) {
        hsv.x = target_hue;
    }
   fragColor = vec4(hsv2rgb(hsv), outColor.a);
}
@end

@program program vs fs
