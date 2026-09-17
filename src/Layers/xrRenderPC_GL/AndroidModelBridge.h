#pragma once

#include <cstdint>
#include <string>
#include <vector>

class IReader;
class IRenderable;
class IRender_ObjectSpecific;
class IRender_Light;
class IRender_Glow;
class IRenderVisual;

struct AndroidRenderModelPool;

struct AndroidModelVertex
{
    float x{}, y{}, z{};
    float nx{}, ny{}, nz{};
    float u{}, v{};
};

struct AndroidModelGeometry
{
    IRenderVisual* source{};
    std::string texture;
    std::vector<AndroidModelVertex> vertices;
    std::vector<std::uint16_t> indices;
};

AndroidRenderModelPool* CreateAndroidRenderModelPool();
void DestroyAndroidRenderModelPool(AndroidRenderModelPool*& pool);
IRenderVisual* AndroidModelCreate(AndroidRenderModelPool* pool, const char* name, IReader* data);
IRenderVisual* AndroidModelCreateChild(AndroidRenderModelPool* pool, const char* name, IReader* data);
IRenderVisual* AndroidModelDuplicate(AndroidRenderModelPool* pool, IRenderVisual* visual);
void AndroidModelDelete(AndroidRenderModelPool* pool, IRenderVisual*& visual, bool discard);
void AndroidModelSetLogging(AndroidRenderModelPool* pool, bool enabled);
void AndroidModelsPrefetch(AndroidRenderModelPool* pool);
void AndroidModelsClear(AndroidRenderModelPool* pool, bool complete);
bool AndroidBuildModelGeometry(IRenderVisual* visual, std::vector<AndroidModelGeometry>& geometry);
IRender_ObjectSpecific* AndroidRenderObjectCreate();
void AndroidRenderObjectDestroy(IRender_ObjectSpecific*& object);
void AndroidRenderObjectUpdate(IRender_ObjectSpecific* object, IRenderable* parent);
IRender_Light* AndroidRenderLightCreate();
void AndroidRenderLightDestroyed(IRender_Light* light);
IRender_Glow* AndroidRenderGlowCreate();
void AndroidRenderGlowDestroyed(IRender_Glow* glow);
