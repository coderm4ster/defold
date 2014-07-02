#include "comp_spine_model.h"

#include <string.h>
#include <float.h>
#include <algorithm>

#include <dlib/array.h>
#include <dlib/hash.h>
#include <dlib/log.h>
#include <dlib/message.h>
#include <dlib/profile.h>
#include <dlib/dstrings.h>
#include <dlib/index_pool.h>
#include <dlib/math.h>
#include <graphics/graphics.h>
#include <render/render.h>
#include <gameobject/gameobject_ddf.h>

#include "../gamesys.h"
#include "../gamesys_private.h"

#include "spine_ddf.h"
#include "sprite_ddf.h"
#include "tile_ddf.h"

using namespace Vectormath::Aos;

namespace dmGameSystem
{
    using namespace Vectormath::Aos;
    using namespace dmGameSystemDDF;

    static const dmhash_t NULL_ANIMATION = dmHashString64("");

    static const dmhash_t PROP_SKIN = dmHashString64("skin");
    static const dmhash_t PROP_ANIMATION = dmHashString64("animation");

    struct SpineModelVertex
    {
        float x;
        float y;
        float z;
        float u;
        float v;
    };

    struct SpineModelWorld
    {
        dmArray<SpineModelComponent>    m_Components;
        dmIndexPool32                   m_ComponentIndices;
        dmArray<dmRender::RenderObject> m_RenderObjects;
        dmGraphics::HVertexDeclaration  m_VertexDeclaration;
        dmGraphics::HVertexBuffer       m_VertexBuffer;
        dmArray<SpineModelVertex>       m_VertexBufferData;

        dmArray<uint32_t>               m_RenderSortBuffer;
        float                           m_MinZ;
        float                           m_MaxZ;
    };

    dmGameObject::CreateResult CompSpineModelNewWorld(const dmGameObject::ComponentNewWorldParams& params)
    {
        SpineModelContext* context = (SpineModelContext*)params.m_Context;
        dmRender::HRenderContext render_context = context->m_RenderContext;
        SpineModelWorld* world = new SpineModelWorld();

        world->m_Components.SetCapacity(context->m_MaxSpineModelCount);
        world->m_Components.SetSize(context->m_MaxSpineModelCount);
        memset(&world->m_Components[0], 0, sizeof(SpineModelComponent) * context->m_MaxSpineModelCount);
        world->m_ComponentIndices.SetCapacity(context->m_MaxSpineModelCount);
        world->m_RenderObjects.SetCapacity(context->m_MaxSpineModelCount);

        world->m_RenderSortBuffer.SetCapacity(context->m_MaxSpineModelCount);
        world->m_RenderSortBuffer.SetSize(context->m_MaxSpineModelCount);
        for (uint32_t i = 0; i < context->m_MaxSpineModelCount; ++i)
        {
            world->m_RenderSortBuffer[i] = i;
        }
        world->m_MinZ = 0;
        world->m_MaxZ = 0;

        dmGraphics::VertexElement ve[] =
        {
                {"position", 0, 3, dmGraphics::TYPE_FLOAT, false},
                {"texcoord0", 1, 2, dmGraphics::TYPE_FLOAT, false},
        };

        world->m_VertexDeclaration = dmGraphics::NewVertexDeclaration(dmRender::GetGraphicsContext(render_context), ve, sizeof(ve) / sizeof(dmGraphics::VertexElement));

        world->m_VertexBuffer = dmGraphics::NewVertexBuffer(dmRender::GetGraphicsContext(render_context), 0, 0x0, dmGraphics::BUFFER_USAGE_STREAM_DRAW);
        // Assume 4 vertices per mesh
        world->m_VertexBufferData.SetCapacity(4 * world->m_Components.Capacity());

        *params.m_World = world;
        return dmGameObject::CREATE_RESULT_OK;
    }

    dmGameObject::CreateResult CompSpineModelDeleteWorld(const dmGameObject::ComponentDeleteWorldParams& params)
    {
        SpineModelWorld* world = (SpineModelWorld*)params.m_World;
        dmGraphics::DeleteVertexDeclaration(world->m_VertexDeclaration);
        dmGraphics::DeleteVertexBuffer(world->m_VertexBuffer);

        delete world;
        return dmGameObject::CREATE_RESULT_OK;
    }

    static bool GetSender(SpineModelComponent* component, dmMessage::URL* out_sender)
    {
        dmMessage::URL sender;
        sender.m_Socket = dmGameObject::GetMessageSocket(dmGameObject::GetCollection(component->m_Instance));
        if (dmMessage::IsSocketValid(sender.m_Socket))
        {
            dmGameObject::Result go_result = dmGameObject::GetComponentId(component->m_Instance, component->m_ComponentIndex, &sender.m_Fragment);
            if (go_result == dmGameObject::RESULT_OK)
            {
                sender.m_Path = dmGameObject::GetIdentifier(component->m_Instance);
                *out_sender = sender;
                return true;
            }
        }
        return false;
    }

    static dmGameSystemDDF::SpineAnimation* FindAnimation(dmGameSystemDDF::AnimationSet* anim_set, dmhash_t animation_id)
    {
        uint32_t anim_count = anim_set->m_Animations.m_Count;
        for (uint32_t i = 0; i < anim_count; ++i)
        {
            dmGameSystemDDF::SpineAnimation* anim = &anim_set->m_Animations[i];
            if (anim->m_Id == animation_id)
            {
                return anim;
            }
        }
        return 0x0;
    }

    static SpinePlayer* GetPlayer(SpineModelComponent* component)
    {
        return &component->m_Players[component->m_CurrentPlayer];
    }

    static SpinePlayer* GetSecondaryPlayer(SpineModelComponent* component)
    {
        return &component->m_Players[(component->m_CurrentPlayer+1) % 2];
    }

    static SpinePlayer* SwitchPlayer(SpineModelComponent* component)
    {
        component->m_CurrentPlayer = (component->m_CurrentPlayer + 1) % 2;
        return &component->m_Players[component->m_CurrentPlayer];
    }

    static bool PlayAnimation(SpineModelComponent* component, dmhash_t animation_id, dmGameObject::Playback playback, float blend_duration)
    {
        dmGameSystemDDF::SpineAnimation* anim = FindAnimation(&component->m_Resource->m_Scene->m_SpineScene->m_AnimationSet, animation_id);
        if (anim != 0x0)
        {
            if (blend_duration > 0.0f)
            {
                component->m_BlendTimer = 0.0f;
                component->m_BlendDuration = blend_duration;
                component->m_Blending = 1;
            }
            else
            {
                SpinePlayer* player = GetPlayer(component);
                player->m_Playing = 0;
            }
            SpinePlayer* player = SwitchPlayer(component);
            player->m_AnimationId = animation_id;
            player->m_Animation = anim;
            player->m_Cursor = 0.0f;
            player->m_Playing = 1;
            player->m_Playback = playback;
            if (player->m_Playback == dmGameObject::PLAYBACK_ONCE_BACKWARD || player->m_Playback == dmGameObject::PLAYBACK_LOOP_BACKWARD)
                player->m_Backwards = 1;
            else
                player->m_Backwards = 0;
            return true;
        }
        return false;
    }

    static void CancelAnimation(SpineModelComponent* component)
    {
        SpinePlayer* player = GetPlayer(component);
        player->m_Playing = 0;
    }

    static void ReHash(SpineModelComponent* component)
    {
        // Hash resource-ptr, material-handle, blend mode and render constants
        HashState32 state;
        bool reverse = false;
        SpineModelResource* resource = component->m_Resource;
        dmGameSystemDDF::SpineModelDesc* ddf = resource->m_Model;
        dmHashInit32(&state, reverse);
        dmHashUpdateBuffer32(&state, &resource, sizeof(resource));
        dmHashUpdateBuffer32(&state, &resource->m_Material, sizeof(resource->m_Material));
        dmHashUpdateBuffer32(&state, &ddf->m_BlendMode, sizeof(ddf->m_BlendMode));
        dmArray<dmRender::Constant>& constants = component->m_RenderConstants;
        uint32_t size = constants.Size();
        // Padding in the SetConstant-struct forces us to copy the components by hand
        for (uint32_t i = 0; i < size; ++i)
        {
            dmRender::Constant& c = constants[i];
            dmHashUpdateBuffer32(&state, &c.m_NameHash, sizeof(uint64_t));
            dmHashUpdateBuffer32(&state, &c.m_Value, sizeof(Vector4));
            component->m_PrevRenderConstants[i] = c.m_Value;
        }
        component->m_MixedHash = dmHashFinal32(&state);
    }

    dmGameObject::CreateResult CompSpineModelCreate(const dmGameObject::ComponentCreateParams& params)
    {
        SpineModelWorld* world = (SpineModelWorld*)params.m_World;

        if (world->m_ComponentIndices.Remaining() == 0)
        {
            dmLogError("Spine Model could not be created since the buffer is full (%d).", world->m_Components.Capacity());
            return dmGameObject::CREATE_RESULT_UNKNOWN_ERROR;
        }
        uint32_t index = world->m_ComponentIndices.Pop();
        SpineModelComponent* component = &world->m_Components[index];
        component->m_Instance = params.m_Instance;
        component->m_Transform = dmTransform::Transform(Vector3(params.m_Position), params.m_Rotation, 1.0f);
        component->m_Resource = (SpineModelResource*)params.m_Resource;
        dmMessage::ResetURL(component->m_Listener);
        component->m_ComponentIndex = params.m_ComponentIndex;
        component->m_Enabled = 1;
        component->m_Mesh = 0x0;
        component->m_Skin = dmHashString64(component->m_Resource->m_Model->m_Skin);
        dmGameSystemDDF::MeshSet* mesh_set = &component->m_Resource->m_Scene->m_SpineScene->m_MeshSet;
        for (uint32_t i = 0; i < mesh_set->m_Meshes.m_Count; ++i)
        {
            dmGameSystemDDF::Mesh* mesh = &mesh_set->m_Meshes[i];
            if (mesh->m_Id == component->m_Skin)
            {
                component->m_Mesh = mesh;
                break;
            }
        }
        dmArray<SpineBone>& bind_pose = component->m_Resource->m_Scene->m_BindPose;
        dmGameSystemDDF::Skeleton* skeleton = &component->m_Resource->m_Scene->m_SpineScene->m_Skeleton;
        uint32_t bone_count = skeleton->m_Bones.m_Count;
        component->m_Pose.SetCapacity(bone_count);
        component->m_Pose.SetSize(bone_count);
        for (uint32_t i = 0; i < bone_count; ++i)
        {
            component->m_Pose[i].SetIdentity();
        }
        component->m_NodeInstances.SetCapacity(bone_count);
        component->m_NodeInstances.SetSize(bone_count);
        memset(component->m_NodeInstances.Begin(), 0, sizeof(dmGameObject::HInstance) * bone_count);
        for (uint32_t i = 0; i < bone_count; ++i)
        {
            dmGameObject::HInstance inst = dmGameObject::New(params.m_Collection, 0x0);
            if (inst == 0x0)
                return dmGameObject::CREATE_RESULT_UNKNOWN_ERROR;
            dmGameObject::SetIdentifier(params.m_Collection, inst, dmGameObject::GenerateUniqueInstanceId(params.m_Collection));
            dmGameObject::SetBone(inst, true);
            dmTransform::Transform transform = bind_pose[i].m_LocalToParent;
            if (i == 0)
            {
                transform = dmTransform::Mul(component->m_Transform, transform);
            }
            dmGameObject::SetPosition(inst, Point3(transform.GetTranslation()));
            dmGameObject::SetRotation(inst, transform.GetRotation());
            dmGameObject::SetScale(inst, transform.GetScale());
            component->m_NodeInstances[i] = inst;
        }
        // Set parents in reverse to account for child-prepending
        for (uint32_t i = 0; i < bone_count; ++i)
        {
            uint32_t index = bone_count - 1 - i;
            dmGameObject::HInstance inst = component->m_NodeInstances[index];
            dmGameObject::HInstance parent = params.m_Instance;
            if (index > 0)
            {
                parent = component->m_NodeInstances[skeleton->m_Bones[index].m_Parent];
            }
            dmGameObject::SetParent(inst, parent);
        }

        ReHash(component);
        dmhash_t default_animation_id = dmHashString64(component->m_Resource->m_Model->m_DefaultAnimation);
        if (default_animation_id != NULL_ANIMATION)
        {
            // Loop forward should be the most common for idle anims etc.
            PlayAnimation(component, default_animation_id, dmGameObject::PLAYBACK_LOOP_FORWARD, 0.0f);
        }

        *params.m_UserData = (uintptr_t)component;
        return dmGameObject::CREATE_RESULT_OK;
    }

    dmGameObject::CreateResult CompSpineModelDestroy(const dmGameObject::ComponentDestroyParams& params)
    {
        SpineModelWorld* world = (SpineModelWorld*)params.m_World;
        SpineModelComponent* component = (SpineModelComponent*)*params.m_UserData;
        // Delete bone game objects
        uint32_t bone_count = component->m_NodeInstances.Size();
        for (uint32_t i = 0; i < bone_count; ++i)
        {
            dmGameObject::HInstance inst = component->m_NodeInstances[i];
            if (inst != 0x0)
            {
                dmGameObject::Delete(params.m_Collection, inst);
            }
            else
            {
                break;
            }
        }
        uint32_t index = component - &world->m_Components[0];
        memset(component, 0, sizeof(SpineModelComponent));
        world->m_ComponentIndices.Push(index);
        return dmGameObject::CREATE_RESULT_OK;
    }

    struct SortPredSpine
    {
        SortPredSpine(SpineModelWorld* world) : m_World(world) {}

        SpineModelWorld* m_World;

        inline bool operator () (const uint32_t x, const uint32_t y)
        {
            SpineModelComponent* c1 = &m_World->m_Components[x];
            SpineModelComponent* c2 = &m_World->m_Components[y];
            return c1->m_SortKey.m_Key < c2->m_SortKey.m_Key;
        }

    };

    static void GenerateKeys(SpineModelWorld* world)
    {
        dmArray<SpineModelComponent>& components = world->m_Components;
        uint32_t n = components.Size();

        float min_z = world->m_MinZ;
        float range = 1.0f / (world->m_MaxZ - world->m_MinZ);

        SpineModelComponent* first = world->m_Components.Begin();
        for (uint32_t i = 0; i < n; ++i)
        {
            SpineModelComponent* c = &components[i];
            uint32_t index = c - first;
            if (c->m_Resource && c->m_Enabled)
            {
                float z = (c->m_World.getElem(3, 2) - min_z) * range * 65535;
                z = dmMath::Clamp(z, 0.0f, 65535.0f);
                uint16_t zf = (uint16_t) z;
                c->m_SortKey.m_Z = zf;
                c->m_SortKey.m_Index = index;
                c->m_SortKey.m_MixedHash = c->m_MixedHash;
            }
            else
            {
                c->m_SortKey.m_Key = 0xffffffffffffffffULL;
            }
        }
    }

    static void Sort(SpineModelWorld* world)
    {
        DM_PROFILE(SpineModel, "Sort");
        dmArray<uint32_t>* buffer = &world->m_RenderSortBuffer;
        SortPredSpine pred(world);
        std::sort(buffer->Begin(), buffer->End(), pred);
    }

    static void CreateVertexData(SpineModelWorld* world, dmArray<SpineModelVertex>& vertex_buffer, TextureSetResource* texture_set, uint32_t start_index, uint32_t end_index)
    {
        DM_PROFILE(SpineModel, "CreateVertexData");

        const dmArray<SpineModelComponent>& components = world->m_Components;
        const dmArray<uint32_t>& sort_buffer = world->m_RenderSortBuffer;

        for (uint32_t i = start_index; i < end_index; ++i)
        {
            const SpineModelComponent* component = &components[sort_buffer[i]];

            dmGameSystemDDF::Mesh* mesh = component->m_Mesh ;
            if (mesh == 0x0)
                continue;

            const Matrix4& w = component->m_World;

            uint32_t index_count = mesh->m_Indices.m_Count;
            for (uint32_t ii = 0; ii < index_count; ++ii)
            {
                vertex_buffer.SetSize(vertex_buffer.Size()+1);
                SpineModelVertex& v = vertex_buffer.Back();
                uint32_t vi = mesh->m_Indices[ii];
                uint32_t e = vi*3;
                Point3 in_p(mesh->m_Positions[e++], mesh->m_Positions[e++], mesh->m_Positions[e++]);
                Point3 out_p(0.0f, 0.0f, 0.0f);
                uint32_t bi_offset = vi * 4;
                uint32_t* bone_indices = &mesh->m_BoneIndices[bi_offset];
                float* bone_weights = &mesh->m_Weights[bi_offset];
                for (uint32_t bi = 0; bi < 4; ++bi)
                {
                    uint32_t bone_index = bone_indices[bi];
                    out_p += Vector3(dmTransform::Apply(component->m_Pose[bone_index], in_p)) * bone_weights[bi];
                }
                *((Vector4*)&v) = w * out_p;
                e = vi*2;
                v.u = mesh->m_Texcoord0[e++];
                v.v = mesh->m_Texcoord0[e++];
            }
        }
    }

    static uint32_t RenderBatch(SpineModelWorld* world, dmRender::HRenderContext render_context, dmArray<SpineModelVertex>& vertex_buffer, uint32_t start_index)
    {
        DM_PROFILE(SpineModel, "RenderBatch");
        uint32_t n = world->m_Components.Size();

        const dmArray<SpineModelComponent>& components = world->m_Components;
        const dmArray<uint32_t>& sort_buffer = world->m_RenderSortBuffer;

        const SpineModelComponent* first = &components[sort_buffer[start_index]];
        assert(first->m_Enabled);
        TextureSetResource* texture_set = first->m_Resource->m_Scene->m_TextureSet;
        uint64_t z = first->m_SortKey.m_Z;
        uint32_t hash = first->m_MixedHash;

        uint32_t vertex_count = 0;
        uint32_t end_index = n;
        for (uint32_t i = start_index; i < n; ++i)
        {
            const SpineModelComponent* c = &components[sort_buffer[i]];
            if (!c->m_Enabled || c->m_MixedHash != hash || c->m_SortKey.m_Z != z)
            {
                end_index = i;
                break;
            }
            if (c->m_Mesh != 0x0)
                vertex_count += c->m_Mesh->m_Indices.m_Count;
        }

        if (vertex_buffer.Remaining() < vertex_count)
            vertex_buffer.OffsetCapacity(vertex_count - vertex_buffer.Remaining());

        // Render object
        dmRender::RenderObject ro;
        ro.m_VertexDeclaration = world->m_VertexDeclaration;
        ro.m_VertexBuffer = world->m_VertexBuffer;
        ro.m_PrimitiveType = dmGraphics::PRIMITIVE_TRIANGLES;
        ro.m_VertexStart = vertex_buffer.Size();
        ro.m_VertexCount = vertex_count;
        ro.m_Material = first->m_Resource->m_Material;
        ro.m_Textures[0] = texture_set->m_Texture;
        // The first transform is used for the batch. Mean-value might be better?
        // NOTE: The position is already transformed, see CreateVertexData, but set for sorting.
        // See also sprite.vp
        ro.m_WorldTransform = first->m_World;
        ro.m_CalculateDepthKey = 1;

        const dmArray<dmRender::Constant>& constants = first->m_RenderConstants;
        uint32_t size = constants.Size();
        for (uint32_t i = 0; i < size; ++i)
        {
            const dmRender::Constant& c = constants[i];
            dmRender::EnableRenderObjectConstant(&ro, c.m_NameHash, c.m_Value);
        }

        dmGameSystemDDF::SpineModelDesc::BlendMode blend_mode = first->m_Resource->m_Model->m_BlendMode;
        switch (blend_mode)
        {
            case dmGameSystemDDF::SpineModelDesc::BLEND_MODE_ALPHA:
                ro.m_SourceBlendFactor = dmGraphics::BLEND_FACTOR_ONE;
                ro.m_DestinationBlendFactor = dmGraphics::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;

            case dmGameSystemDDF::SpineModelDesc::BLEND_MODE_ADD:
                ro.m_SourceBlendFactor = dmGraphics::BLEND_FACTOR_ONE;
                ro.m_DestinationBlendFactor = dmGraphics::BLEND_FACTOR_ONE;
            break;

            case dmGameSystemDDF::SpineModelDesc::BLEND_MODE_MULT:
                ro.m_SourceBlendFactor = dmGraphics::BLEND_FACTOR_DST_COLOR;
                ro.m_DestinationBlendFactor = dmGraphics::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;

            default:
                dmLogError("Unknown blend mode: %d\n", blend_mode);
                assert(0);
            break;
        }
        ro.m_SetBlendFactors = 1;

        world->m_RenderObjects.Push(ro);
        dmRender::AddToRender(render_context, &world->m_RenderObjects[world->m_RenderObjects.Size() - 1]);

        CreateVertexData(world, vertex_buffer, texture_set, start_index, end_index);
        return end_index;
    }

    void UpdateTransforms(SpineModelWorld* world)
    {
        DM_PROFILE(SpineModel, "UpdateTransforms");

        dmArray<SpineModelComponent>& components = world->m_Components;
        uint32_t n = components.Size();
        float min_z = FLT_MAX;
        float max_z = -FLT_MAX;
        for (uint32_t i = 0; i < n; ++i)
        {
            SpineModelComponent* c = &components[i];

            // NOTE: texture_set = c->m_Resource might be NULL so it's essential to "continue" here
            if (!c->m_Enabled)
                continue;

            if (c->m_Mesh != 0x0)
            {
                dmTransform::Transform world = dmGameObject::GetWorldTransform(c->m_Instance);
                if (dmGameObject::ScaleAlongZ(c->m_Instance))
                {
                    world = dmTransform::Mul(world, c->m_Transform);
                }
                else
                {
                    world = dmTransform::MulNoScaleZ(world, c->m_Transform);
                }
                Matrix4 w = dmTransform::ToMatrix4(world);
                Vector4 position = w.getCol3();
                float z = position.getZ();
                min_z = dmMath::Min(min_z, z);
                max_z = dmMath::Max(max_z, z);
                w.setCol3(position);
                c->m_World = w;
            }
        }

        if (n == 0)
        {
            // NOTE: Avoid large numbers and risk of de-normalized etc.
            // if n == 0 the actual values of min/max-z doens't matter
            min_z = 0;
            max_z = 1;
        }

        world->m_MinZ = min_z;
        world->m_MaxZ = max_z;
    }

    static Vector3 SampleVec3(uint32_t sample, float frac, float* data)
    {
        uint32_t i = sample*3;
        return lerp(frac, Vector3(data[i+0], data[i+1], data[i+2]), Vector3(data[i+0+3], data[i+1+3], data[i+2+3]));
    }

    static Quat SampleQuat(uint32_t sample, float frac, float* data)
    {
        uint32_t i = sample*4;
        return lerp(frac, Quat(data[i+0], data[i+1], data[i+2], data[i+3]), Quat(data[i+0+4], data[i+1+4], data[i+2+4], data[i+3+4]));
    }

    static float CursorToTime(float cursor, float duration, bool backwards, bool once_pingpong)
    {
        float t = cursor;
        if (backwards)
            t = duration - t;
        if (once_pingpong && t > duration * 0.5f)
        {
            t = duration - t;
        }
        return t;
    }

    static void PostEvent(dmMessage::URL* sender, dmMessage::URL* receiver, dmhash_t event_id, dmGameSystemDDF::EventKey* key)
    {
        dmGameSystemDDF::SpineEvent event;
        event.m_EventId = event_id;
        event.m_T = key->m_T;
        event.m_Integer = key->m_Integer;
        event.m_Float = key->m_Float;
        event.m_String = key->m_String;

        dmhash_t message_id = dmGameSystemDDF::SpineEvent::m_DDFDescriptor->m_NameHash;

        uintptr_t descriptor = (uintptr_t)dmGameSystemDDF::SpineEvent::m_DDFDescriptor;
        uint32_t data_size = sizeof(dmGameSystemDDF::SpineEvent);
        dmMessage::Result result = dmMessage::Post(sender, receiver, message_id, 0, descriptor, &event, data_size);
        if (result != dmMessage::RESULT_OK)
        {
            dmLogError("Could not send spine_event to listener.");
        }
    }

    static void PostEventsInterval(dmMessage::URL* sender, dmMessage::URL* receiver, dmGameSystemDDF::SpineAnimation* animation, float start_cursor, float end_cursor, float duration, bool backwards)
    {
        const uint32_t track_count = animation->m_EventTracks.m_Count;
        for (uint32_t ti = 0; ti < track_count; ++ti)
        {
            dmGameSystemDDF::EventTrack* track = &animation->m_EventTracks[ti];
            const uint32_t key_count = track->m_Keys.m_Count;
            for (uint32_t ki = 0; ki < key_count; ++ki)
            {
                dmGameSystemDDF::EventKey* key = &track->m_Keys[ki];
                float cursor = key->m_T;
                if (backwards)
                    cursor = duration - cursor;
                if (start_cursor <= cursor && cursor < end_cursor)
                {
                    PostEvent(sender, receiver, track->m_EventId, key);
                }
            }
        }
    }

    static void PostEvents(SpinePlayer* player, dmMessage::URL* sender, dmMessage::URL* listener, dmGameSystemDDF::SpineAnimation* animation, float dt, float prev_cursor, float duration, bool completed)
    {
        dmMessage::URL receiver = *listener;
        if (!dmMessage::IsSocketValid(receiver.m_Socket))
        {
            receiver = *sender;
            receiver.m_Fragment = 0; // broadcast to sibling components
        }
        float cursor = player->m_Cursor;
        // Since the intervals are defined as t0 <= t < t1, make sure we include the end of the animation, i.e. when t1 == duration
        if (completed)
            cursor += dt;
        // If the start cursor is greater than the end cursor, we have looped and handle that as two distinct intervals: [0,end_cursor) and [start_cursor,duration)
        // Note that for looping ping pong, one event can be triggered twice during the same frame by appearing in both intervals
        if (prev_cursor > cursor)
        {
            bool prev_backwards = player->m_Backwards;
            // Handle the flipping nature of ping pong
            if (player->m_Playback == dmGameObject::PLAYBACK_LOOP_PINGPONG)
            {
                prev_backwards = !player->m_Backwards;
            }
            PostEventsInterval(sender, &receiver, animation, prev_cursor, duration, duration, prev_backwards);
            PostEventsInterval(sender, &receiver, animation, 0.0f, cursor, duration, player->m_Backwards);
        }
        else
        {
            // Special handling when we reach the way back of once ping pong playback
            float half_duration = duration * 0.5f;
            if (player->m_Playback == dmGameObject::PLAYBACK_ONCE_PINGPONG && cursor > half_duration)
            {
                // If the previous cursor was still in the forward direction, treat it as two distinct intervals: [start_cursor,half_duration) and [half_duration, end_cursor)
                if (prev_cursor < half_duration)
                {
                    PostEventsInterval(sender, &receiver, animation, prev_cursor, half_duration, duration, false);
                    PostEventsInterval(sender, &receiver, animation, half_duration, cursor, duration, true);
                }
                else
                {
                    PostEventsInterval(sender, &receiver, animation, prev_cursor, cursor, duration, true);
                }
            }
            else
            {
                PostEventsInterval(sender, &receiver, animation, prev_cursor, cursor, duration, player->m_Backwards);
            }
        }
    }

    static float GetCursorDuration(SpinePlayer* player, dmGameSystemDDF::SpineAnimation* animation)
    {
        float duration = animation->m_Duration;
        if (player->m_Playback == dmGameObject::PLAYBACK_ONCE_PINGPONG)
        {
            duration *= 2.0f;
        }
        return duration;
    }

    static void UpdatePlayer(SpineModelComponent* component, SpinePlayer* player, float dt, dmMessage::URL* listener)
    {
        dmGameSystemDDF::SpineAnimation* animation = player->m_Animation;
        if (animation == 0x0 || !player->m_Playing)
            return;

        // Advance cursor
        float prev_cursor = player->m_Cursor;
        if (player->m_Playback != dmGameObject::PLAYBACK_NONE)
        {
            player->m_Cursor += dt;
        }
        float duration = GetCursorDuration(player, animation);

        // Adjust cursor
        bool completed = false;
        switch (player->m_Playback)
        {
        case dmGameObject::PLAYBACK_ONCE_FORWARD:
        case dmGameObject::PLAYBACK_ONCE_BACKWARD:
        case dmGameObject::PLAYBACK_ONCE_PINGPONG:
            if (player->m_Cursor >= duration)
            {
                player->m_Cursor = duration;
                completed = true;
            }
            break;
        case dmGameObject::PLAYBACK_LOOP_FORWARD:
        case dmGameObject::PLAYBACK_LOOP_BACKWARD:
            while (player->m_Cursor >= duration)
            {
                player->m_Cursor -= duration;
            }
            break;
        case dmGameObject::PLAYBACK_LOOP_PINGPONG:
            while (player->m_Cursor >= duration)
            {
                player->m_Cursor -= duration;
                player->m_Backwards = ~player->m_Backwards;
            }
            break;
        default:
            break;
        }

        dmMessage::URL sender;
        if (prev_cursor != player->m_Cursor && GetSender(component, &sender))
        {
            dmMessage::URL receiver = *listener;
            receiver.m_Function = 0;
            PostEvents(player, &sender, &receiver, animation, dt, prev_cursor, duration, completed);
        }

        if (completed)
        {
            player->m_Playing = 0;
            // Only report completeness for the primary player
            if (player == GetPlayer(component) && dmMessage::IsSocketValid(listener->m_Socket))
            {
                dmMessage::URL sender;
                if (GetSender(component, &sender))
                {
                    dmhash_t message_id = dmGameSystemDDF::SpineAnimationDone::m_DDFDescriptor->m_NameHash;
                    dmGameSystemDDF::SpineAnimationDone message;
                    message.m_AnimationId = player->m_AnimationId;
                    message.m_Playback = player->m_Playback;

                    dmMessage::URL receiver = *listener;
                    uintptr_t descriptor = (uintptr_t)dmGameSystemDDF::SpineAnimationDone::m_DDFDescriptor;
                    uint32_t data_size = sizeof(dmGameSystemDDF::SpineAnimationDone);
                    dmMessage::Result result = dmMessage::Post(&sender, &receiver, message_id, 0, descriptor, &message, data_size);
                    dmMessage::ResetURL(*listener);
                    if (result != dmMessage::RESULT_OK)
                    {
                        dmLogError("Could not send animation_done to listener.");
                    }
                }
                else
                {
                    dmLogError("Could not send animation_done to listener because of incomplete component.");
                }
            }
        }
    }

    static void UpdatePose(SpinePlayer* player, dmArray<dmTransform::Transform>& pose, float blend_weight)
    {
        dmGameSystemDDF::SpineAnimation* animation = player->m_Animation;
        if (animation == 0x0)
            return;
        float duration = GetCursorDuration(player, animation);
        float t = CursorToTime(player->m_Cursor, duration, player->m_Backwards, player->m_Playback == dmGameObject::PLAYBACK_ONCE_PINGPONG);

        float fraction = t * animation->m_SampleRate;
        uint32_t sample = (uint32_t)fraction;
        fraction -= sample;
        // Sample animation tracks
        uint32_t track_count = animation->m_Tracks.m_Count;
        for (uint32_t ti = 0; ti < track_count; ++ti)
        {
            dmGameSystemDDF::AnimationTrack* track = &animation->m_Tracks[ti];
            uint32_t bone_index = track->m_BoneIndex;
            dmTransform::Transform& transform = pose[bone_index];
            if (track->m_Positions.m_Count > 0)
            {
                transform.SetTranslation(lerp(blend_weight, transform.GetTranslation(), SampleVec3(sample, fraction, track->m_Positions.m_Data)));
            }
            if (track->m_Rotations.m_Count > 0)
            {
                transform.SetRotation(lerp(blend_weight, transform.GetRotation(), SampleQuat(sample, fraction, track->m_Rotations.m_Data)));
            }
            if (track->m_Scale.m_Count > 0)
            {
                transform.SetScale(lerp(blend_weight, transform.GetScale(), SampleVec3(sample, fraction, track->m_Scale.m_Data)));
            }
        }
    }

    static void UpdateBlend(SpineModelComponent* component, float dt)
    {
        if (component->m_Blending)
        {
            component->m_BlendTimer += dt;
            if (component->m_BlendTimer >= component->m_BlendDuration)
            {
                component->m_Blending = 0;
                SpinePlayer* secondary = GetSecondaryPlayer(component);
                secondary->m_Playing = 0;
            }
        }
    }

    static void Animate(SpineModelWorld* world, float dt)
    {
        DM_PROFILE(SpineModel, "Animate");

        dmArray<SpineModelComponent>& components = world->m_Components;
        uint32_t n = components.Size();
        for (uint32_t i = 0; i < n; ++i)
        {
            SpineModelComponent* component = &components[i];
            if (!component->m_Enabled || component->m_Pose.Empty())
                continue;

            dmGameSystemDDF::Skeleton* skeleton = &component->m_Resource->m_Scene->m_SpineScene->m_Skeleton;
            dmArray<SpineBone>& bind_pose = component->m_Resource->m_Scene->m_BindPose;
            dmArray<dmTransform::Transform>& pose = component->m_Pose;
            // Reset pose
            uint32_t bone_count = pose.Size();
            for (uint32_t bi = 0; bi < bone_count; ++bi)
            {
                pose[bi].SetIdentity();
            }

            UpdateBlend(component, dt);

            SpinePlayer* player = GetPlayer(component);
            if (component->m_Blending)
            {
                float fade_rate = component->m_BlendTimer / component->m_BlendDuration;
                float blend_weight = 1.0f;
                for (uint32_t pi = 0; pi < 2; ++pi)
                {
                    SpinePlayer* p = &component->m_Players[pi];
                    UpdatePlayer(component, p, dt, &component->m_Listener);
                    UpdatePose(p, pose, blend_weight);
                    if (player == p)
                    {
                        blend_weight = 1.0f - fade_rate;
                    }
                    else
                    {
                        blend_weight = fade_rate;
                    }
                }
            }
            else
            {
                UpdatePlayer(component, player, dt, &component->m_Listener);
                UpdatePose(player, pose, 1.0f);
            }

            for (uint32_t bi = 0; bi < bone_count; ++bi)
            {
                dmTransform::Transform& t = pose[bi];
                // Normalize quaternions while we blend
                if (component->m_Blending)
                {
                    Quat rotation = t.GetRotation();
                    if (dot(rotation, rotation) > 0.001f)
                        rotation = normalize(rotation);
                    pose[bi].SetRotation(rotation);
                }
                pose[bi] = dmTransform::Mul(bind_pose[bi].m_LocalToParent, pose[bi]);
            }

            // Include component transform in the GO instance reflecting the root bone
            dmTransform::Transform root_t = pose[0];
            pose[0] = dmTransform::Mul(component->m_Transform, root_t);
            dmGameObject::SetBoneTransforms(component->m_Instance, pose.Begin(), pose.Size());
            pose[0] = root_t;
            for (uint32_t bi = 0; bi < bone_count; ++bi)
            {
                dmTransform::Transform& transform = pose[bi];
                // Convert every transform into model space
                if (bi > 0) {
                    transform = dmTransform::Mul(pose[skeleton->m_Bones[bi].m_Parent], transform);
                }
            }
            for (uint32_t bi = 0; bi < bone_count; ++bi)
            {
                dmTransform::Transform& transform = pose[bi];
                // Multiply by inv bind pose to obtain delta transforms
                transform = dmTransform::Mul(transform, bind_pose[bi].m_ModelToLocal);
            }
        }
    }

    dmGameObject::UpdateResult CompSpineModelUpdate(const dmGameObject::ComponentsUpdateParams& params)
    {
        /*
         * All spine models are sorted, using the m_RenderSortBuffer, with respect to the:
         *
         *     - hash value of m_Resource, i.e. equal iff the sprite is rendering with identical atlas
         *     - z-value
         *     - component index
         *  or
         *     - 0xffffffff (or corresponding 64-bit value) if not enabled
         * such that all non-enabled spine models ends up last in the array
         * and spine models with equal atlas and depth consecutively
         *
         * The z-sorting is considered a hack as we assume a camera pointing along the z-axis. We currently
         * have no access, by design as render-data currently should be invariant to camera parameters,
         * to the transformation matrices when generating render-data. The render-system and go-system should probably
         * be changed such that unique render-objects are created when necessary and on-demand instead of up-front as
         * currently. Another option could be a call-back when the actual rendering occur.
         *
         * The sorted array of indices are grouped into batches, using z and resource-hash as predicates, and every
         * batch is rendered using a single draw-call. Note that the world transform
         * is set to first sprite transform for correct batch sorting. The actual vertex transformation is performed in code
         * and standard world-transformation is removed from vertex-program.
         *
         * NOTES:
         * * When/if transparency the batching predicates must be updated in order to
         *   support per sprite correct sorting.
         */

        SpineModelContext* context = (SpineModelContext*)params.m_Context;
        dmRender::HRenderContext render_context = context->m_RenderContext;
        SpineModelWorld* world = (SpineModelWorld*)params.m_World;

        dmGraphics::SetVertexBufferData(world->m_VertexBuffer, 6 * sizeof(SpineModelVertex) * world->m_Components.Size(), 0x0, dmGraphics::BUFFER_USAGE_DYNAMIC_DRAW);
        dmArray<SpineModelVertex>& vertex_buffer = world->m_VertexBufferData;
        vertex_buffer.SetSize(0);

        dmArray<SpineModelComponent>& components = world->m_Components;
        uint32_t sprite_count = world->m_Components.Size();
        for (uint32_t i = 0; i < sprite_count; ++i)
        {
            SpineModelComponent& component = components[i];
            if (!component.m_Enabled)
                continue;
            uint32_t const_count = component.m_RenderConstants.Size();
            for (uint32_t const_i = 0; const_i < const_count; ++const_i)
            {
                float diff_sq = lengthSqr(component.m_RenderConstants[const_i].m_Value - component.m_PrevRenderConstants[const_i]);
                if (diff_sq > 0)
                {
                    ReHash(&component);
                    break;
                }
            }
        }
        UpdateTransforms(world);
        GenerateKeys(world);
        Sort(world);

        world->m_RenderObjects.SetSize(0);

        const dmArray<uint32_t>& sort_buffer = world->m_RenderSortBuffer;

        Animate(world, params.m_UpdateContext->m_DT);

        uint32_t start_index = 0;
        uint32_t n = components.Size();
        while (start_index < n && components[sort_buffer[start_index]].m_Enabled)
        {
            start_index = RenderBatch(world, render_context, vertex_buffer, start_index);
        }

        void* vertex_buffer_data = 0x0;
        if (!vertex_buffer.Empty())
            vertex_buffer_data = (void*)&(vertex_buffer[0]);
        dmGraphics::SetVertexBufferData(world->m_VertexBuffer, vertex_buffer.Size() * sizeof(SpineModelVertex), vertex_buffer_data, dmGraphics::BUFFER_USAGE_DYNAMIC_DRAW);

        return dmGameObject::UPDATE_RESULT_OK;
    }

    static bool CompSpineModelGetConstantCallback(void* user_data, dmhash_t name_hash, dmRender::Constant** out_constant)
    {
        SpineModelComponent* component = (SpineModelComponent*)user_data;
        dmArray<dmRender::Constant>& constants = component->m_RenderConstants;
        uint32_t count = constants.Size();
        for (uint32_t i = 0; i < count; ++i)
        {
            dmRender::Constant& c = constants[i];
            if (c.m_NameHash == name_hash)
            {
                *out_constant = &c;
                return true;
            }
        }
        return false;
    }

    static void CompSpineModelSetConstantCallback(void* user_data, dmhash_t name_hash, uint32_t* element_index, const dmGameObject::PropertyVar& var)
    {
        SpineModelComponent* component = (SpineModelComponent*)user_data;
        dmArray<dmRender::Constant>& constants = component->m_RenderConstants;
        uint32_t count = constants.Size();
        Vector4* v = 0x0;
        for (uint32_t i = 0; i < count; ++i)
        {
            dmRender::Constant& c = constants[i];
            if (c.m_NameHash == name_hash)
            {
                v = &c.m_Value;
                break;
            }
        }
        if (v == 0x0)
        {
            if (constants.Full())
            {
                uint32_t capacity = constants.Capacity() + 4;
                constants.SetCapacity(capacity);
                component->m_PrevRenderConstants.SetCapacity(capacity);
            }
            dmRender::Constant c;
            dmRender::GetMaterialProgramConstant(component->m_Resource->m_Material, name_hash, c);
            constants.Push(c);
            component->m_PrevRenderConstants.Push(c.m_Value);
            v = &(constants[constants.Size()-1].m_Value);
        }
        if (element_index == 0x0)
            *v = Vector4(var.m_V4[0], var.m_V4[1], var.m_V4[2], var.m_V4[3]);
        else
            v->setElem(*element_index, (float)var.m_Number);
        ReHash(component);
    }

    dmGameObject::UpdateResult CompSpineModelOnMessage(const dmGameObject::ComponentOnMessageParams& params)
    {
        SpineModelComponent* component = (SpineModelComponent*)*params.m_UserData;
        if (params.m_Message->m_Id == dmGameObjectDDF::Enable::m_DDFDescriptor->m_NameHash)
        {
            component->m_Enabled = 1;
        }
        else if (params.m_Message->m_Id == dmGameObjectDDF::Disable::m_DDFDescriptor->m_NameHash)
        {
            component->m_Enabled = 0;
        }
        else if (params.m_Message->m_Descriptor != 0x0)
        {
            if (params.m_Message->m_Id == dmGameSystemDDF::SpinePlayAnimation::m_DDFDescriptor->m_NameHash)
            {
                dmGameSystemDDF::SpinePlayAnimation* ddf = (dmGameSystemDDF::SpinePlayAnimation*)params.m_Message->m_Data;
                if (PlayAnimation(component, ddf->m_AnimationId, (dmGameObject::Playback)ddf->m_Playback, ddf->m_BlendDuration))
                {
                    component->m_Listener = params.m_Message->m_Sender;
                }
            }
            else if (params.m_Message->m_Id == dmGameSystemDDF::SpineCancelAnimation::m_DDFDescriptor->m_NameHash)
            {
                CancelAnimation(component);
            }
        }

        return dmGameObject::UPDATE_RESULT_OK;
    }

    void CompSpineModelOnReload(const dmGameObject::ComponentOnReloadParams& params)
    {
        // TODO Implement in DEF-375
    }

    dmGameObject::PropertyResult CompSpineModelGetProperty(const dmGameObject::ComponentGetPropertyParams& params, dmGameObject::PropertyDesc& out_value)
    {
        SpineModelComponent* component = (SpineModelComponent*)*params.m_UserData;
        if (params.m_PropertyId == PROP_SKIN)
        {
            out_value.m_Variant = dmGameObject::PropertyVar(component->m_Skin);
            return dmGameObject::PROPERTY_RESULT_OK;
        }
        else if (params.m_PropertyId == PROP_ANIMATION)
        {
            SpinePlayer* player = GetPlayer(component);
            out_value.m_Variant = dmGameObject::PropertyVar(player->m_Animation);
            return dmGameObject::PROPERTY_RESULT_OK;
        }
        return GetMaterialConstant(component->m_Resource->m_Material, params.m_PropertyId, out_value, CompSpineModelGetConstantCallback, component);
    }

    dmGameObject::PropertyResult CompSpineModelSetProperty(const dmGameObject::ComponentSetPropertyParams& params)
    {
        SpineModelComponent* component = (SpineModelComponent*)*params.m_UserData;
        if (params.m_PropertyId == PROP_SKIN)
        {
            if (params.m_Value.m_Type != dmGameObject::PROPERTY_TYPE_HASH)
                return dmGameObject::PROPERTY_RESULT_TYPE_MISMATCH;
            dmGameSystemDDF::Mesh* mesh = 0x0;
            dmGameSystemDDF::MeshSet* mesh_set = &component->m_Resource->m_Scene->m_SpineScene->m_MeshSet;
            dmhash_t skin = params.m_Value.m_Hash;
            for (uint32_t i = 0; i < mesh_set->m_Meshes.m_Count; ++i)
            {
                if (skin == mesh_set->m_Meshes[i].m_Id)
                {
                    mesh = &mesh_set->m_Meshes[i];
                    break;
                }
            }
            if (mesh == 0x0)
            {
                dmLogError("Could not find skin '%s' in the mesh set.", (const char*)dmHashReverse64(skin, 0x0));
                return dmGameObject::PROPERTY_RESULT_NOT_FOUND;
            }
            component->m_Mesh = mesh;
            component->m_Skin = params.m_Value.m_Hash;
            return dmGameObject::PROPERTY_RESULT_OK;
        }
        return SetMaterialConstant(component->m_Resource->m_Material, params.m_PropertyId, params.m_Value, CompSpineModelSetConstantCallback, component);
    }
}
