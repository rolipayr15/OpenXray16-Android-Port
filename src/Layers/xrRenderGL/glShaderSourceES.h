#pragma once

// OpenGL ES 3.2 support for the GLSL 4.10 shaders in gamedata/shaders/gl.
// The shaders are shared with the desktop renderer; the differences are
// handled here so the shader sources stay a single tree.

// Lines that replace "#version 410" and the desktop extension directives.
pcstr glShaderPreambleES();

// Rewrites one source fragment for GLSL ES. stage is the engine target letter
// ('v' vertex, 'p' pixel, ...):
// - fragment outputs get explicit locations (desktop binds them with
//   glBindFragDataLocation, which GLES does not have);
// - float varyings between the vertex and pixel stage become vec4, keeping
//   the original name as a swizzle macro, because ES links stages by
//   location and requires identical types.
xr_string glRewriteShaderSourceES(pcstr source, char stage);
