#include <assert.h>

#include <dlib/hash.h>
#include <dlib/profile.h>

#include <ddf/ddf.h>

#include <graphics/graphics_device.h>

#include "render_private.h"
#include "debug_renderer.h"
#include "font_renderer.h"
#include "render/mesh_ddf.h"

#include "model/model.h"

namespace dmRender
{
    RenderType::RenderType()
    : m_BeginCallback(0x0)
    , m_DrawCallback(0x0)
    , m_EndCallback(0x0)
    , m_UserContext(0x0)
    {

    }

    RenderContextParams::RenderContextParams()
    : m_MaxRenderTypes(0)
    , m_MaxInstances(0)
    , m_VertexProgramData(0x0)
    , m_VertexProgramDataSize(0)
    , m_FragmentProgramData(0x0)
    , m_FragmentProgramDataSize(0)
    , m_MaxCharacters(0)
    {

    }

    HRenderContext NewRenderContext(const RenderContextParams& params)
    {
        RenderContext* context = new RenderContext;

        context->m_RenderTypes.SetCapacity(params.m_MaxRenderTypes);
        context->m_RenderTargets.SetCapacity(params.m_MaxRenderTargets);

        context->m_RenderObjects.SetCapacity(params.m_MaxInstances);
        context->m_RenderObjects.SetSize(0);

        context->m_GFXContext = dmGraphics::GetContext();

        context->m_View = Vectormath::Aos::Matrix4::identity();
        context->m_Projection = Vectormath::Aos::Matrix4::identity();

        InitializeDebugRenderer(context, params.m_VertexProgramData, params.m_VertexProgramDataSize, params.m_FragmentProgramData, params.m_FragmentProgramDataSize);

        context->m_DisplayWidth = params.m_DisplayWidth;
        context->m_DisplayHeight = params.m_DisplayHeight;

        InitializeTextContext(context, params.m_MaxCharacters);
        return context;
    }

    Result DeleteRenderContext(HRenderContext render_context)
    {
        if (render_context == 0x0) return RESULT_INVALID_CONTEXT;

        FinalizeDebugRenderer(render_context);
        FinalizeTextContext(render_context);
        delete render_context;

        return RESULT_OK;
    }

    Result RegisterRenderType(HRenderContext render_context, RenderType render_type, HRenderType* out_render_type)
    {
        if (render_context == 0x0)
            return RESULT_INVALID_CONTEXT;
        if (render_context->m_RenderTypes.Full())
            return RESULT_BUFFER_IS_FULL;
        render_context->m_RenderTypes.Push(render_type);
        *out_render_type = render_context->m_RenderTypes.Size() - 1;
        return RESULT_OK;
    }

    Result RegisterRenderTarget(HRenderContext render_context, dmGraphics::HRenderTarget rendertarget, uint32_t hash)
    {
        if (render_context == 0x0)
            return RESULT_INVALID_CONTEXT;
        if (render_context->m_RenderTargets.Full())
            return RESULT_BUFFER_IS_FULL;

        RenderTargetSetup setup;
        setup.m_RenderTarget = rendertarget;
        setup.m_Hash = hash;
        render_context->m_RenderTargets.Push(setup);

        return RESULT_OK;
    }

    dmGraphics::HRenderTarget GetRenderTarget(HRenderContext render_context, uint32_t hash)
    {
        for (uint32_t i=0; i < render_context->m_RenderTargets.Size(); i++)
        {
            if (render_context->m_RenderTargets[i].m_Hash == hash)
                return render_context->m_RenderTargets[i].m_RenderTarget;
        }

        return 0x0;
    }

    dmGraphics::HContext GetGraphicsContext(HRenderContext render_context)
    {
        return render_context->m_GFXContext;
    }

    Matrix4* GetViewProjectionMatrix(HRenderContext render_context)
    {
        return &render_context->m_ViewProj;
    }

    void SetViewMatrix(HRenderContext render_context, const Vectormath::Aos::Matrix4& view)
    {
        render_context->m_View = view;
        render_context->m_ViewProj = render_context->m_Projection * view;
    }

    void SetProjectionMatrix(HRenderContext render_context, const Vectormath::Aos::Matrix4& projection)
    {
        render_context->m_Projection = projection;
        render_context->m_ViewProj = projection * render_context->m_View;
    }

    uint32_t GetDisplayWidth(HRenderContext render_context)
    {
        return render_context->m_DisplayWidth;
    }

    uint32_t GetDisplayHeight(HRenderContext render_context)
    {
        return render_context->m_DisplayHeight;
    }

    Result AddToRender(HRenderContext context, HRenderObject ro)
    {
        if (context == 0x0) return RESULT_INVALID_CONTEXT;
        if (context->m_RenderObjects.Full())
        {
            if (!context->m_OutOfResources)
            {
                dmLogWarning("Renderer is out of resources, some objects will not be rendered.");
                context->m_OutOfResources = 1;
            }
            return RESULT_OUT_OF_RESOURCES;
        }
        context->m_RenderObjects.Push(ro);

        return RESULT_OK;
    }

    Result ClearRenderObjects(HRenderContext context)
    {
        context->m_RenderObjects.SetSize(0);
        ClearDebugRenderObjects(context);

        context->m_TextContext.m_RenderObjectIndex = 0;
        context->m_TextContext.m_Vertices.SetSize(0);

        return RESULT_OK;
    }

    Result Draw(HRenderContext render_context, Predicate* predicate)
    {
        if (render_context == 0x0)
            return RESULT_INVALID_CONTEXT;
        uint32_t tag_mask = 0;
        if (predicate != 0x0)
            tag_mask = ConvertMaterialTagsToMask(&predicate->m_Tags[0], predicate->m_TagCount);
        uint32_t type = ~0u;

        for (uint32_t i = 0; i < render_context->m_RenderObjects.Size(); ++i)
        {
            HRenderObject ro = render_context->m_RenderObjects[i];
            if ((GetMaterialTagMask(ro->m_Material) & tag_mask) == tag_mask)
            {
                dmGraphics::HContext context = dmRender::GetGraphicsContext(render_context);
                dmGraphics::SetFragmentProgram(context, GetMaterialFragmentProgram(dmRender::GetMaterial(ro)));
                dmGraphics::SetVertexProgram(context, GetMaterialVertexProgram(dmRender::GetMaterial(ro)));

                void* user_context = render_context->m_RenderTypes[ro->m_Type].m_UserContext;
                // check if we need to change render type and run its setup func
                if (type != ro->m_Type)
                {
                    if (render_context->m_RenderTypes[ro->m_Type].m_BeginCallback)
                        render_context->m_RenderTypes[ro->m_Type].m_BeginCallback(render_context, user_context);
                    type = ro->m_Type;
                }

                uint32_t material_vertex_mask = GetMaterialVertexConstantMask(ro->m_Material);
                uint32_t material_fragment_mask = GetMaterialFragmentConstantMask(ro->m_Material);
                for (uint32_t j = 0; j < MAX_CONSTANT_COUNT; ++j)
                {
                    uint32_t mask = 1 << j;
                    if (ro->m_VertexConstantMask & mask)
                    {
                        dmGraphics::SetVertexConstantBlock(context, &ro->m_VertexConstants[j], j, 1);
                    }
                    else if (material_vertex_mask & mask)
                    {
                        switch (GetMaterialVertexProgramConstantType(ro->m_Material, j))
                        {
                            case dmRenderDDF::MaterialDesc::CONSTANT_TYPE_USER:
                            {
                                Vector4 constant = GetMaterialVertexProgramConstant(ro->m_Material, j);
                                dmGraphics::SetVertexConstantBlock(context, &constant, j, 1);
                                break;
                            }
                            case dmRenderDDF::MaterialDesc::CONSTANT_TYPE_VIEWPROJ:
                            {
                                dmGraphics::SetVertexConstantBlock(context, (Vector4*)&render_context->m_ViewProj, j, 4);
                                break;
                            }
                            case dmRenderDDF::MaterialDesc::CONSTANT_TYPE_WORLD:
                            {
                                dmGraphics::SetVertexConstantBlock(context, (Vector4*)&ro->m_WorldTransform, j, 4);
                                break;
                            }
                            case dmRenderDDF::MaterialDesc::CONSTANT_TYPE_TEXTURE:
                            {
                                dmGraphics::SetVertexConstantBlock(context, (Vector4*)&ro->m_TextureTransform, j, 4);
                                break;
                            }
                        }
                    }
                    if (ro->m_FragmentConstantMask & (1 << j))
                    {
                        dmGraphics::SetFragmentConstant(context, &ro->m_FragmentConstants[j], j);
                    }
                    else if (material_fragment_mask & mask)
                    {
                        switch (GetMaterialFragmentProgramConstantType(ro->m_Material, j))
                        {
                            case dmRenderDDF::MaterialDesc::CONSTANT_TYPE_USER:
                            {
                                Vector4 constant = GetMaterialFragmentProgramConstant(ro->m_Material, j);
                                dmGraphics::SetFragmentConstantBlock(context, &constant, j, 1);
                                break;
                            }
                            case dmRenderDDF::MaterialDesc::CONSTANT_TYPE_VIEWPROJ:
                            {
                                dmGraphics::SetFragmentConstantBlock(context, (Vector4*)&render_context->m_ViewProj, j, 4);
                                break;
                            }
                            case dmRenderDDF::MaterialDesc::CONSTANT_TYPE_WORLD:
                            {
                                dmGraphics::SetFragmentConstantBlock(context, (Vector4*)&ro->m_WorldTransform, j, 4);
                                break;
                            }
                            case dmRenderDDF::MaterialDesc::CONSTANT_TYPE_TEXTURE:
                            {
                                dmGraphics::SetFragmentConstantBlock(context, (Vector4*)&ro->m_TextureTransform, j, 4);
                                break;
                            }
                        }
                    }
                }

                // dispatch
                if (render_context->m_RenderTypes[ro->m_Type].m_DrawCallback)
                    render_context->m_RenderTypes[ro->m_Type].m_DrawCallback(render_context, user_context, ro, 1);

                if (i == render_context->m_RenderObjects.Size() - 1 || type != render_context->m_RenderObjects[i+1]->m_Type)
                {
                    if (render_context->m_RenderTypes[ro->m_Type].m_EndCallback)
                        render_context->m_RenderTypes[ro->m_Type].m_EndCallback(render_context, user_context);
                }
            }
        }
        return RESULT_OK;
    }

    Result DrawDebug3d(HRenderContext context)
    {
        return Draw(context, &context->m_DebugRenderer.m_3dPredicate);
    }

    Result DrawDebug2d(HRenderContext context)
    {
        return Draw(context, &context->m_DebugRenderer.m_2dPredicate);
    }

    HRenderObject NewRenderObject(uint32_t type, HMaterial material)
    {
        RenderObject* ro = new RenderObject;
        ro->m_WorldTransform = Matrix4::identity();
        ro->m_TextureTransform = Matrix4::identity();
        ro->m_Material = 0;
        ro->m_UserData = 0x0;
        ro->m_Type = type;
        ro->m_VertexConstantMask = 0;
        ro->m_FragmentConstantMask = 0;

        SetMaterial(ro, material);

        return ro;
    }

    void DeleteRenderObject(HRenderObject ro)
    {
        delete ro;
    }

    void SetVertexConstant(HRenderObject ro, uint32_t reg, const Vectormath::Aos::Vector4& value)
    {
        if (reg < MAX_CONSTANT_COUNT)
        {
            ro->m_VertexConstants[reg] = value;
            ro->m_VertexConstantMask |= 1 << reg;
        }
        else
        {
            dmLogWarning("Illegal register (%d) supplied as vertex constant.", reg);
        }
    }

    void ResetVertexConstant(HRenderObject ro, uint32_t reg)
    {
        if (reg < MAX_CONSTANT_COUNT)
            ro->m_VertexConstantMask &= ~(1 << reg);
        else
            dmLogWarning("Illegal register (%d) supplied as vertex constant.", reg);
    }

    void SetFragmentConstant(HRenderObject ro, uint32_t reg, const Vectormath::Aos::Vector4& value)
    {
        if (reg < MAX_CONSTANT_COUNT)
        {
            ro->m_FragmentConstants[reg] = value;
            ro->m_FragmentConstantMask |= 1 << reg;
        }
        else
        {
            dmLogWarning("Illegal register (%d) supplied as fragment constant.", reg);
        }
    }

    void ResetFragmentConstant(HRenderObject ro, uint32_t reg)
    {
        if (reg < MAX_CONSTANT_COUNT)
            ro->m_FragmentConstantMask &= ~(1 << reg);
        else
            dmLogWarning("Illegal register (%d) supplied as fragment constant.", reg);
    }

    const Matrix4* GetWorldTransform(HRenderObject ro)
    {
        return &ro->m_WorldTransform;
    }

    void SetWorldTransform(HRenderObject ro, const Matrix4& world_transform)
    {
        ro->m_WorldTransform = world_transform;
    }

    const Matrix4* GetTextureTransform(HRenderObject ro)
    {
        return &ro->m_TextureTransform;
    }

    void SetTextureTransform(HRenderObject ro, const Matrix4& texture_transform)
    {
        ro->m_TextureTransform = texture_transform;
    }

    void* GetUserData(HRenderObject ro)
    {
        return ro->m_UserData;
    }

    void SetUserData(HRenderObject ro, void* user_data)
    {
        ro->m_UserData = user_data;
    }

    HMaterial GetMaterial(HRenderObject ro)
    {
        return ro->m_Material;
    }

    void SetMaterial(HRenderObject ro, HMaterial material)
    {
        ro->m_Material = material;
    }
}
