#version 450

layout(location = 0) in vec4 vertexColor;
layout(location = 1) in vec2 vertexTexcoord;
layout(location = 0) out vec4 outputColor;

layout(set = 0, binding = 0) uniform sampler2D uiTexture;

layout(push_constant) uniform UiConstants
{
    vec2 size;
    uint textureMode;
} constants;

void main()
{
    vec4 sampled = texture(uiTexture, vertexTexcoord);
    if (constants.textureMode == 1u)
        // ShoC HUD/font atlases are DXT5 DDS files. After GLI expands them
        // to RGBA8 their RGB channels are solid white and the glyph coverage
        // lives in alpha, so sampling red produces opaque character quads.
        outputColor = vec4(vertexColor.rgb, sampled.a * vertexColor.a);
    else if (constants.textureMode == 2u)
        outputColor = vec4(sampled.rgb, (1.0 - sampled.a) * vertexColor.a);
    else
        outputColor = sampled * vertexColor;
}
