#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 texcoord;

layout(location = 0) out vec3 vertexNormal;
layout(location = 1) out vec2 vertexTexcoord;

layout(push_constant) uniform WorldConstants
{
    mat4 worldViewProjection;
} constants;

void main()
{
    gl_Position = constants.worldViewProjection * vec4(position, 1.0);
    vertexNormal = normal;
    vertexTexcoord = texcoord;
}
