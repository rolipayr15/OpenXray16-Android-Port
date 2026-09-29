#version 450

layout(location = 0) in vec3 vertexNormal;
layout(location = 1) in vec2 vertexTexcoord;
layout(location = 2) in vec2 vertexLightmapTexcoord;
layout(location = 3) in vec2 vertexDetailTexcoord;
layout(location = 0) out vec4 outputColor;

layout(set = 0, binding = 0) uniform sampler2D baseTexture;
layout(set = 1, binding = 0) uniform sampler2D lightmapTexture;
layout(set = 2, binding = 0) uniform sampler2D hemiTexture;
layout(set = 3, binding = 0) uniform sampler2D detailTexture;

layout(push_constant) uniform WorldConstants
{
    mat4 worldViewProjection;
    uint materialFlags;
    float detailScale;
    vec4 ambientAndSunDirectionX;
    vec4 hemiAndSunDirectionY;
    vec4 sunColorAndDirectionZ;
} constants;

void main()
{
    vec3 n = normalize(vertexNormal);
    vec4 base = texture(baseTexture, vertexTexcoord);
    if ((constants.materialFlags & 8u) != 0u && base.a < 0.18)
        discard;

    if ((constants.materialFlags & 1u) != 0u)
    {
        vec4 lightmap = texture(lightmapTexture, vertexLightmapTexcoord);
        float hemiOcclusion = (constants.materialFlags & 2u) != 0u ?
            texture(hemiTexture, vertexLightmapTexcoord).a : 1.0;
        vec3 sunDirection = normalize(vec3(constants.ambientAndSunDirectionX.a,
            constants.hemiAndSunDirectionY.a, constants.sunColorAndDirectionZ.a));
        float sunAmount = max(dot(n, -sunDirection), 0.0) * lightmap.a;
        vec3 lighting = constants.ambientAndSunDirectionX.rgb + lightmap.rgb +
            constants.hemiAndSunDirectionY.rgb * hemiOcclusion +
            constants.sunColorAndDirectionZ.rgb * sunAmount;
        outputColor = vec4(base.rgb * lighting * 2.0, base.a);
    }
    else
    {
        vec3 sunDirection = normalize(vec3(constants.ambientAndSunDirectionX.a,
            constants.hemiAndSunDirectionY.a, constants.sunColorAndDirectionZ.a));
        float sunAmount = max(dot(n, -sunDirection), 0.0);
        float hemiAmount = 0.35 + 0.65 * max(n.y, 0.0);
        vec3 lighting = constants.ambientAndSunDirectionX.rgb +
            constants.hemiAndSunDirectionY.rgb * hemiAmount +
            constants.sunColorAndDirectionZ.rgb * sunAmount;
        outputColor = vec4(base.rgb * max(lighting * 2.0, vec3(0.12)), base.a);
    }

    if ((constants.materialFlags & 64u) != 0u)
    {
        vec3 detail = texture(detailTexture, vertexDetailTexcoord).rgb * 2.0;
        float footprint = max(length(dFdx(vertexDetailTexcoord)), length(dFdy(vertexDetailTexcoord)));
        float detailWeight = 1.0 - smoothstep(0.08, 0.35, footprint);
        outputColor.rgb *= mix(vec3(1.0), detail, detailWeight);
    }
}
