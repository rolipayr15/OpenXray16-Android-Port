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
    // X-Ray's projection reaches the Android Vulkan surface rotated by 180
    // degrees.  Keep UI coordinates untouched and correct only the 3D pass.
    gl_Position.xy = -gl_Position.xy;
    vertexNormal = normal;
    vertexTexcoord = texcoord;
}
