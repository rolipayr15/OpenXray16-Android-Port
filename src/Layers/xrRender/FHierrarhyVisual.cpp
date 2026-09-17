// FHierrarhyVisual.cpp: implementation of the FHierrarhyVisual class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#pragma hdrstop

#include "FHierrarhyVisual.h"
#include "xrCore/FMesh.hpp"
#include "xrEngine/Render.h"
#ifdef _EDITOR
#include "Include/xrAPI/xrAPI.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

FHierrarhyVisual::FHierrarhyVisual() : dxRender_Visual() { bDontDelete = FALSE; }
FHierrarhyVisual::~FHierrarhyVisual()
{
    if (!bDontDelete)
    {
        for (auto& child : children)
#ifdef __ANDROID__
            GEnv.Render->model_Delete((IRenderVisual*&)child, false);
#else
            RImplementation.model_Delete((IRenderVisual*&)child, false);
#endif
    }
    children.clear();
}

void FHierrarhyVisual::Release()
{
    if (!bDontDelete)
    {
        for (u32 i = 0; i < children.size(); i++)
            children[i]->Release();
    }
}

void FHierrarhyVisual::Load(const char* N, IReader* data, u32 dwFlags)
{
    dxRender_Visual::Load(N, data, dwFlags);
    if (data->find_chunk(OGF_CHILDREN_L))
    {
        // From Link
        u32 cnt = data->r_u32();
        children.resize(cnt);
        for (u32 i = 0; i < cnt; i++)
        {
#ifdef _EDITOR
            THROW;
#else
            u32 ID = data->r_u32();
#ifdef __ANDROID__
            children[i] = (dxRender_Visual*)GEnv.Render->getVisual(ID);
#else
            children[i] = (dxRender_Visual*)RImplementation.getVisual(ID);
#endif
#endif
        }
        bDontDelete = TRUE;
    }
    else
    {
        if (data->find_chunk(OGF_CHILDREN))
        {
            // From stream
            IReader* OBJ = data->open_chunk(OGF_CHILDREN);
            if (OBJ)
            {
                IReader* O = OBJ->open_chunk(0);
                for (int count = 1; O; count++)
                {
                    string_path name_load, short_name, num;
                    xr_strcpy(short_name, N);
                    if (strext(short_name))
                        *strext(short_name) = 0;
                    strconcat(sizeof(name_load), name_load, short_name, ":", xr_itoa(count, num, 10));
#ifdef __ANDROID__
                    children.push_back((dxRender_Visual*)GEnv.Render->model_CreateChild(name_load, O));
#else
                    children.push_back((dxRender_Visual*)RImplementation.model_CreateChild(name_load, O));
#endif
                    O->close();
                    O = OBJ->open_chunk(count);
                }
                OBJ->close();
            }
            bDontDelete = FALSE;
        }
        else
        {
            FATAL("Invalid visual");
        }
    }
}

void FHierrarhyVisual::Copy(dxRender_Visual* pSrc)
{
    dxRender_Visual::Copy(pSrc);

    FHierrarhyVisual* pFrom = (FHierrarhyVisual*)pSrc;

    children.clear();
    children.reserve(pFrom->children.size());
    for (u32 i = 0; i < pFrom->children.size(); i++)
    {
#ifdef __ANDROID__
        dxRender_Visual* p = (dxRender_Visual*)GEnv.Render->model_Duplicate(pFrom->children[i]);
#else
        dxRender_Visual* p = (dxRender_Visual*)RImplementation.model_Duplicate(pFrom->children[i]);
#endif
        children.push_back(p);
    }
    bDontDelete = FALSE;
}
} // namespace xray::render::RENDER_NAMESPACE
