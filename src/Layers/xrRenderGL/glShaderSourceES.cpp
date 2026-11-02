#include "stdafx.h"

#include "glShaderSourceES.h"

#include <cctype>

pcstr glShaderPreambleES()
{
    // misc/android/validate-gles-shaders.py reads this block verbatim; keep
    // one string literal per line between the markers.
    static constexpr char preamble[] =
        // BEGIN ES PREAMBLE
        "#version 320 es\n"
        "#extension GL_EXT_clip_cull_distance : enable\n"
        "#extension GL_EXT_texture_cube_map_array : enable\n"
        "precision highp float;\n"
        "precision highp int;\n"
        "precision highp sampler2D;\n"
        "precision highp sampler3D;\n"
        "precision highp samplerCube;\n"
        "precision highp sampler2DShadow;\n"
        "precision highp sampler2DArray;\n"
        "precision highp sampler2DArrayShadow;\n"
        "precision highp samplerCubeShadow;\n"
        "precision highp sampler2DMS;\n"
        "precision highp isampler2D;\n"
        "precision highp usampler2D;\n"
        "#define OPENXRAY_GLES 1\n"
        // END ES PREAMBLE
        ;
    return preamble;
}

namespace
{
bool IsIdentifier(char character)
{
    return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
}

size_t SkipSpaces(const xr_string& line, size_t position)
{
    while (position < line.size() && std::isspace(static_cast<unsigned char>(line[position])))
        ++position;
    return position;
}

size_t ReadWord(const xr_string& line, size_t position, xr_string& word)
{
    const size_t start = position;
    while (position < line.size() && IsIdentifier(line[position]))
        ++position;
    word.assign(line, start, position - start);
    return position;
}

// Components of a float varying type the shaders use (HLSL-style aliases
// included); 0 for anything that must keep its declared type.
int FloatComponents(const xr_string& type)
{
    if (type == "float" || type == "half")
        return 1;
    if (type == "float2" || type == "half2" || type == "vec2")
        return 2;
    if (type == "float3" || type == "half3" || type == "vec3")
        return 3;
    return 0;
}

// "out vec4 SV_TargetN;" -> "layout(location = N) out vec4 SV_TargetN;"
bool RewriteFragmentOutput(const xr_string& line, xr_string& result)
{
    const size_t cursor = SkipSpaces(line, 0);
    const size_t target = line.find("SV_Target");
    if (line.compare(cursor, 4, "out ") != 0 || target == xr_string::npos)
        return false;
    size_t digits = target + 9;
    while (digits < line.size() && std::isdigit(static_cast<unsigned char>(line[digits])))
        ++digits;
    if (digits == target + 9)
        return false;
    result.append(line, 0, cursor);
    result += "layout(location = ";
    result.append(line, target + 9, digits - target - 9);
    result += ") ";
    result.append(line, cursor, xr_string::npos);
    return true;
}

// "layout(location = L) [qualifiers] in|out <float1-3> name;" on a stage
// interface -> a vec4 varying plus a macro that keeps the original name.
// OpenGL ES links stages by location and requires identical types, while the
// HLSL-derived shaders let a pixel shader read fewer components than the
// vertex shader wrote. Widening every float varying to vec4 makes any pair
// that shares a location compatible.
bool RewriteVarying(const xr_string& line, char stage, xr_string& result)
{
    size_t position = SkipSpaces(line, 0);
    if (line.compare(position, 6, "layout") != 0)
        return false;
    const size_t close = line.find(')', position);
    if (close == xr_string::npos || line.find("location", position) > close)
        return false;
    const xr_string layout(line, 0, close + 1);

    xr_string qualifiers;
    xr_string word;
    position = close + 1;
    while (true)
    {
        position = SkipSpaces(line, position);
        const size_t next = ReadWord(line, position, word);
        if (word.empty())
            return false;
        position = next;
        if (word == "in" || word == "out")
            break;
        if (word != "flat" && word != "smooth" && word != "noperspective" && word != "centroid")
            return false;
        qualifiers += word;
        qualifiers += ' ';
    }
    const bool isInput = word == "in";
    // Vertex inputs are attributes and fragment outputs are render targets;
    // only the varyings between the two stages are widened.
    if ((stage == 'v' && isInput) || (stage == 'p' && !isInput) || (stage != 'v' && stage != 'p'))
        return false;

    xr_string type;
    position = ReadWord(line, SkipSpaces(line, position), type);
    const int components = FloatComponents(type);
    if (components == 0 || qualifiers.find("flat") != xr_string::npos)
        return false;

    xr_string name;
    position = ReadWord(line, SkipSpaces(line, position), name);
    position = SkipSpaces(line, position);
    if (name.empty() || position >= line.size() || line[position] != ';')
        return false; // arrays and multi-declarations keep their form

    static constexpr pcstr swizzles[] = { "", ".x", ".xy", ".xyz" };
    const xr_string wide = "oxr_" + name;
    result += layout + " " + qualifiers + (isInput ? "in" : "out") + " vec4 " + wide + ";";
    result.append(line, position + 1, xr_string::npos);
    result += "\n#define " + name + " " + wide + swizzles[components];
    return true;
}
} // namespace

xr_string glRewriteShaderSourceES(pcstr source, char stage)
{
    xr_string result;
    const size_t length = xr_strlen(source);
    result.reserve(length + 256);

    size_t lineStart = 0;
    while (lineStart < length)
    {
        size_t lineEnd = lineStart;
        while (lineEnd < length && source[lineEnd] != '\n')
            ++lineEnd;
        const xr_string line(source + lineStart, lineEnd - lineStart);

        if (!RewriteFragmentOutput(line, result) && !RewriteVarying(line, stage, result))
            result += line;
        if (lineEnd < length)
            result += '\n';
        lineStart = lineEnd + 1;
    }
    return result;
}
