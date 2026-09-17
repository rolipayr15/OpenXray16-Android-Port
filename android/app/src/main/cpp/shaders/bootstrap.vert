#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in uint packedColor;
layout(location = 2) in vec2 texcoord;
layout(location = 0) out vec4 vertexColor;
layout(location = 1) out vec2 vertexTexcoord;

layout(push_constant) uniform UiConstants
{
    vec2 size;
    uint textureMode;
} constants;

void main()
{
    // A positive Vulkan viewport maps NDC -1 to the top edge. X-Ray UI
    // coordinates also start at the top-left, unlike OpenGL's framebuffer.
    vec2 ndc = vec2(position.x * 2.0 / constants.size.x - 1.0,
                    position.y * 2.0 / constants.size.y - 1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vertexColor = vec4(float((packedColor >> 16) & 255u),
                       float((packedColor >> 8) & 255u),
                       float(packedColor & 255u),
                       float((packedColor >> 24) & 255u)) / 255.0;
    vertexTexcoord = texcoord;
}
