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

xr_string glRewriteShaderSourceES(pcstr source)
{
    // "out <type> SV_TargetN;" -> "layout(location = N) out <type> SV_TargetN;"
    xr_string result;
    const size_t length = xr_strlen(source);
    result.reserve(length + 64);

    size_t lineStart = 0;
    while (lineStart < length)
    {
        size_t lineEnd = lineStart;
        while (lineEnd < length && source[lineEnd] != '\n')
            ++lineEnd;
        const xr_string line(source + lineStart, lineEnd - lineStart);

        size_t cursor = 0;
        while (cursor < line.size() && std::isspace(static_cast<unsigned char>(line[cursor])))
            ++cursor;
        const size_t target = line.find("SV_Target");
        bool rewritten = false;
        if (line.compare(cursor, 4, "out ") == 0 && target != xr_string::npos)
        {
            size_t digits = target + 9;
            while (digits < line.size() && std::isdigit(static_cast<unsigned char>(line[digits])))
                ++digits;
            if (digits > target + 9)
            {
                result.append(line, 0, cursor);
                result += "layout(location = ";
                result.append(line, target + 9, digits - target - 9);
                result += ") ";
                result.append(line, cursor, xr_string::npos);
                rewritten = true;
            }
        }
        if (!rewritten)
            result += line;
        if (lineEnd < length)
            result += '\n';
        lineStart = lineEnd + 1;
    }
    return result;
}
