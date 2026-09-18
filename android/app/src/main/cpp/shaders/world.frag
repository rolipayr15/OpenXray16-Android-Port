#version 450

layout(location = 0) in vec3 vertexNormal;
layout(location = 1) in vec2 vertexTexcoord;
layout(location = 0) out vec4 outputColor;

layout(set = 0, binding = 0) uniform sampler2D baseTexture;

void main()
{
    vec3 n = normalize(vertexNormal);
    float light = 0.22 + 0.78 * abs(dot(n, normalize(vec3(0.35, 0.82, 0.45))));
    vec4 base = texture(baseTexture, vertexTexcoord);
    outputColor = vec4(base.rgb * light, base.a);
}
