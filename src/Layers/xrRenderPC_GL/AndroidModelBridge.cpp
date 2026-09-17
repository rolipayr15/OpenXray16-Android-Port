#include "stdafx.h"

#include "AndroidModelBridge.h"
#include "Layers/xrRender/FBasicVisual.h"
#include "Layers/xrRender/FHierrarhyVisual.h"
#include "Layers/xrRender/FSkinned.h"
#include "Layers/xrRender/LightTrack.h"
#include "Layers/xrRender/ModelPool.h"
#include "Layers/xrRender/SkeletonXVertRender.h"
#include "Layers/xrRender/light.h"

#include <algorithm>

using xray::render::render_gl::CModelPool;
using xray::render::render_gl::dxRender_Visual;
using xray::render::render_gl::CROS_impl;
using xray::render::render_gl::FHierrarhyVisual;
using xray::render::render_gl::CSkeletonX_ST;
using xray::render::render_gl::CSkeletonX_PM;
using xray::render::render_gl::vertRender;

namespace
{
class AndroidRenderGlow final : public IRender_Glow
{
public:
    void set_active(bool value) override { active = value; }
    bool get_active() override { return active; }
    void set_position(const Fvector& value) override { position = value; }
    void set_direction(const Fvector& value) override { direction.normalize_safe(value); }
    void set_radius(float value) override { radius = std::max(value, 0.f); }
    void set_texture(pcstr value) override { texture = value && value[0] ? value : nullptr; }
    void set_color(const Fcolor& value) override { color = value; }
    void set_color(float r, float g, float b) override { color.set(r, g, b, 1.f); }

    bool active{};
    Fvector position{};
    Fvector direction{0.f, 0.f, 1.f};
    float radius{};
    Fcolor color{1.f, 1.f, 1.f, 1.f};
    shared_str texture;
};

xr_vector<IRender_Light*> androidLights;
xr_vector<IRender_Glow*> androidGlows;

template <typename T>
void EraseRenderObject(xr_vector<T*>& objects, T* object)
{
    const auto found = std::find(objects.begin(), objects.end(), object);
    if (found != objects.end())
        objects.erase(found);
}

#ifdef __ANDROID__
bool AppendModelGeometry(dxRender_Visual* visual, std::vector<AndroidModelGeometry>& output)
{
    if (!visual)
        return false;

    if (auto* hierarchy = dynamic_cast<FHierrarhyVisual*>(visual))
    {
        bool appended = false;
        for (dxRender_Visual* child : hierarchy->children)
            appended = AppendModelGeometry(child, output) || appended;
        return appended;
    }

    xr_vector<vertRender> skinnedVertices;
    xr_vector<u16> sourceIndices;
    bool built = false;
    if (auto* skinned = dynamic_cast<CSkeletonX_ST*>(visual))
    {
        if (skinned->has_visible_bones())
            built = skinned->AndroidBuildGeometry(skinnedVertices, sourceIndices);
    }
    else if (auto* skinned = dynamic_cast<CSkeletonX_PM*>(visual))
    {
        if (skinned->has_visible_bones())
            built = skinned->AndroidBuildGeometry(skinnedVertices, sourceIndices);
    }
    if (!built)
        return false;

    AndroidModelGeometry geometry;
    geometry.source = visual;
    geometry.texture = visual->androidTexture.c_str() ? visual->androidTexture.c_str() : "";
    geometry.vertices.resize(skinnedVertices.size());
    for (std::size_t i = 0; i < skinnedVertices.size(); ++i)
    {
        const vertRender& source = skinnedVertices[i];
        AndroidModelVertex& destination = geometry.vertices[i];
        destination.x = source.P.x;
        destination.y = source.P.y;
        destination.z = source.P.z;
        destination.nx = source.N.x;
        destination.ny = source.N.y;
        destination.nz = source.N.z;
        destination.u = source.u;
        destination.v = source.v;
    }
    geometry.indices.assign(sourceIndices.begin(), sourceIndices.end());
    output.push_back(std::move(geometry));
    return true;
}
#endif
} // namespace

struct AndroidRenderModelPool
{
    CModelPool models;
};

AndroidRenderModelPool* CreateAndroidRenderModelPool()
{
    return xr_new<AndroidRenderModelPool>();
}

void DestroyAndroidRenderModelPool(AndroidRenderModelPool*& pool)
{
    xr_delete(pool);
}

IRenderVisual* AndroidModelCreate(AndroidRenderModelPool* pool, const char* name, IReader* data)
{
    return pool ? pool->models.Create(name, data) : nullptr;
}

IRenderVisual* AndroidModelCreateChild(AndroidRenderModelPool* pool, const char* name, IReader* data)
{
    return pool ? pool->models.CreateChild(name, data) : nullptr;
}

IRenderVisual* AndroidModelDuplicate(AndroidRenderModelPool* pool, IRenderVisual* visual)
{
    return pool && visual ? pool->models.Instance_Duplicate(static_cast<dxRender_Visual*>(visual)) : nullptr;
}

void AndroidModelDelete(AndroidRenderModelPool* pool, IRenderVisual*& visual, bool discard)
{
    if (!visual)
        return;
    if (!pool)
    {
        xr_delete(visual);
        return;
    }
    auto* model = static_cast<dxRender_Visual*>(visual);
    pool->models.Delete(model, discard ? TRUE : FALSE);
    visual = nullptr;
}

void AndroidModelSetLogging(AndroidRenderModelPool* pool, bool enabled)
{
    if (pool)
        pool->models.Logging(enabled ? TRUE : FALSE);
}

void AndroidModelsPrefetch(AndroidRenderModelPool* pool)
{
    if (pool)
        pool->models.Prefetch();
}

void AndroidModelsClear(AndroidRenderModelPool* pool, bool complete)
{
    if (pool)
        pool->models.ClearPool(complete ? TRUE : FALSE);
}

bool AndroidBuildModelGeometry(IRenderVisual* visual, std::vector<AndroidModelGeometry>& geometry)
{
    geometry.clear();
#ifdef __ANDROID__
    if (!visual)
        return false;
    if (IKinematics* kinematics = visual->dcast_PKinematics())
        kinematics->CalculateBones();
    return AppendModelGeometry(static_cast<dxRender_Visual*>(visual), geometry);
#else
    (void)visual;
    return false;
#endif
}

IRender_ObjectSpecific* AndroidRenderObjectCreate()
{
    return xr_new<CROS_impl>();
}

void AndroidRenderObjectDestroy(IRender_ObjectSpecific*& object)
{
    xr_delete(object);
}

void AndroidRenderObjectUpdate(IRender_ObjectSpecific* object, IRenderable* parent)
{
    if (object)
        static_cast<CROS_impl*>(object)->update(parent);
}

IRender_Light* AndroidRenderLightCreate()
{
    auto* object = xr_new<xray::render::render_gl::light>();
    androidLights.push_back(object);
    return object;
}

void AndroidRenderLightDestroyed(IRender_Light* object)
{
    EraseRenderObject(androidLights, object);
}

IRender_Glow* AndroidRenderGlowCreate()
{
    auto* object = xr_new<AndroidRenderGlow>();
    androidGlows.push_back(object);
    return object;
}

void AndroidRenderGlowDestroyed(IRender_Glow* object)
{
    EraseRenderObject(androidGlows, object);
}
