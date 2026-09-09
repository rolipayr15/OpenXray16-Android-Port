#version 450

layout(location = 0) out vec3 vertexColor;

const vec2 positions[3] = vec2[](
    vec2( 0.00, -0.62),
    vec2( 0.62,  0.50),
    vec2(-0.62,  0.50)
);

const vec3 colors[3] = vec3[](
    vec3(0.95, 0.68, 0.18),
    vec3(0.22, 0.78, 0.43),
    vec3(0.20, 0.52, 0.92)
);

void main()
{
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    vertexColor = colors[gl_VertexIndex];
}
