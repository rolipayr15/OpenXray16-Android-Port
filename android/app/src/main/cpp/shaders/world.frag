#version 450

layout(location = 0) in vec3 vertexNormal;
layout(location = 1) in vec2 vertexTexcoord;
layout(location = 0) out vec4 outputColor;

void main()
{
    // Until material textures are connected, preserve spatial readability
    // with directional lighting over the real mesh normals.
    vec3 n = normalize(vertexNormal);
    float light = 0.22 + 0.78 * abs(dot(n, normalize(vec3(0.35, 0.82, 0.45))));
    vec3 base = mix(vec3(0.22, 0.28, 0.20), vec3(0.52, 0.48, 0.35),
                    0.5 + 0.5 * sin(vertexTexcoord.x * 6.2831853));
    outputColor = vec4(base * light, 1.0);
}
