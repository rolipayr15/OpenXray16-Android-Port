#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 texcoord;
layout(location = 3) in vec2 lightmapTexcoord;

layout(location = 0) out vec3 vertexNormal;
layout(location = 1) out vec2 vertexTexcoord;
layout(location = 2) out vec2 vertexLightmapTexcoord;

layout(push_constant) uniform WorldConstants
{
    mat4 worldViewProjection;
    uint materialFlags;
} constants;

void main()
{
    gl_Position = constants.worldViewProjection * vec4(position, 1.0);
    // Android's surface convention reverses X-Ray's vertical screen axis.
    // Horizontal input and UI stay in physical screen coordinates; keeping X
    // unchanged also avoids mirroring the world, minimap and HUD weapon.
    gl_Position.y = -gl_Position.y;
    vertexNormal = normal;
    vertexTexcoord = texcoord;
    vertexLightmapTexcoord = lightmapTexcoord;
}
