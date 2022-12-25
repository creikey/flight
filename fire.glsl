@module fire

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

void main() {
    vec4 tex_color = texture(iChannel0,texUV);

    // https://airtightinteractive.com/util/hex-to-glsl/
    float how_faded = clamp((1.0 - tex_color.a) - 0.32, 0.0, 1.0);
    vec3 edge_color = mix(vec3(1.0, 0.0, 0.0), vec3(0.929,0.624,0.439), how_faded);
    vec3 body_color = vec3(0.933,0.788,0.69);
    if(tex_color.a <= 0.05)
    {
        fragColor = vec4(0.0);
    } else {
        fragColor = vec4(mix(edge_color, body_color, tex_color.a), mix(1.0, 0.0, clamp((1.0 - tex_color.a) - 0.2, 0.0, 1.0)));
    }
    /*
    vec3 smoke_body = 
    vec3 smoke_edge = tex_color.rgb;

    if(tex_color.a <= 0.05)
    {
        fragColor = vec4(0.0);
    } else {
        fragColor = vec4(mix(smoke_edge, smoke_body, tex_color.a), 1.0);
    }
    */
    
}
@end

@program program vs fs
