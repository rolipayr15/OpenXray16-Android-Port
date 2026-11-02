#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 texcoord;
layout(location = 3) in vec2 lightmapTexcoord;

layout(location = 0) out vec3 vertexNormal;
layout(location = 1) out vec2 vertexTexcoord;
layout(location = 2) out vec2 vertexLightmapTexcoord;
layout(location = 3) out vec2 vertexDetailTexcoord;
layout(location = 4) out float vertexFog;

// Must match WorldPushConstants in AndroidVulkanBootstrap.cpp.
layout(push_constant) uniform WorldConstants
{
    mat4 worldViewProjection;
    uint materialFlags;
    float detailScale;
    float fogNear;
    float fogFar;
    vec4 ambientAndSunDirectionX;
    vec4 hemiAndSunDirectionY;
    vec4 sunColorAndDirectionZ;
    vec4 normalMatrixCol0; // .w: fog colour red
    vec4 normalMatrixCol1; // .w: fog colour green
    vec4 normalMatrixCol2; // .w: fog colour blue
} constants;

void main()
{
    gl_Position = constants.worldViewProjection * vec4(position, 1.0);
    // Android's surface convention reverses X-Ray's vertical screen axis.
    // Horizontal input and UI stay in physical screen coordinates; keeping X
    // unchanged also avoids mirroring the world, minimap and HUD weapon.
    gl_Position.y = -gl_Position.y;
    // Lighting is evaluated in world space; rotate model-space normals.
    vertexNormal = mat3(constants.normalMatrixCol0.xyz, constants.normalMatrixCol1.xyz,
        constants.normalMatrixCol2.xyz) * normal;
    // w is the view-space depth for X-Ray's perspective projection.
    vertexFog = constants.fogFar > constants.fogNear ?
        clamp((gl_Position.w - constants.fogNear) / (constants.fogFar - constants.fogNear), 0.0, 1.0) : 0.0;
    vertexTexcoord = texcoord;
    vertexLightmapTexcoord = lightmapTexcoord;
    vertexDetailTexcoord = texcoord * constants.detailScale;
}
