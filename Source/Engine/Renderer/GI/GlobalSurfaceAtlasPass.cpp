// Copyright (c) 2012-2022 Wojciech Figat. All rights reserved.

#include "GlobalSurfaceAtlasPass.h"
#include "DynamicDiffuseGlobalIllumination.h"
#include "../GlobalSignDistanceFieldPass.h"
#include "../GBufferPass.h"
#include "../RenderList.h"
#include "../ShadowsPass.h"
#include "Engine/Core/Math/Matrix3x3.h"
#include "Engine/Core/Math/OrientedBoundingBox.h"
#include "Engine/Engine/Engine.h"
#include "Engine/Content/Content.h"
#include "Engine/Graphics/GPUDevice.h"
#include "Engine/Graphics/Graphics.h"
#include "Engine/Graphics/RenderTask.h"
#include "Engine/Graphics/RenderBuffers.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Graphics/Shaders/GPUShader.h"
#include "Engine/Level/Actors/StaticModel.h"
#include "Engine/Level/Scene/SceneRendering.h"
#include "Engine/Renderer/ColorGradingPass.h"
#include "Engine/Renderer/EyeAdaptationPass.h"
#include "Engine/Renderer/PostProcessingPass.h"
#include "Engine/Utilities/RectPack.h"

// This must match HLSL
#define GLOBAL_SURFACE_ATLAS_CHUNKS_RESOLUTION 40 // Amount of chunks (in each direction) to split atlas draw distance for objects culling
#define GLOBAL_SURFACE_ATLAS_CHUNKS_GROUP_SIZE 4
#define GLOBAL_SURFACE_ATLAS_OBJECT_DATA_STRIDE 6 // Amount of float4s per-object
#define GLOBAL_SURFACE_ATLAS_TILE_DATA_STRIDE 5 // Amount of float4s per-tile
#define GLOBAL_SURFACE_ATLAS_TILE_PADDING 1 // 1px padding to prevent color bleeding between tiles
#define GLOBAL_SURFACE_ATLAS_TILE_SIZE_MIN 8 // The minimum size of the tile
#define GLOBAL_SURFACE_ATLAS_TILE_SIZE_MAX 192 // The maximum size of the tile
#define GLOBAL_SURFACE_ATLAS_TILE_PROJ_PLANE_OFFSET 0.1f // Small offset to prevent clipping with the closest triangles (shifts near and far planes)
#define GLOBAL_SURFACE_ATLAS_DEBUG_FORCE_REDRAW_TILES 0 // Forces to redraw all object tiles every frame
#define GLOBAL_SURFACE_ATLAS_DEBUG_DRAW_OBJECTS 0 // Debug draws object bounds on redraw (and tile draw projection locations)
#define GLOBAL_SURFACE_ATLAS_DEBUG_DRAW_CHUNKS 0 // Debug draws culled chunks bounds (non-empty

#if GLOBAL_SURFACE_ATLAS_DEBUG_DRAW_OBJECTS || GLOBAL_SURFACE_ATLAS_DEBUG_DRAW_CHUNKS
#include "Engine/Debug/DebugDraw.h"
#endif

PACK_STRUCT(struct Data0
    {
    Float3 ViewWorldPos;
    float ViewNearPlane;
    float SkyboxIntensity;
    uint32 CulledObjectsCapacity;
    float LightShadowsStrength;
    float ViewFarPlane;
    Float4 ViewFrustumWorldRays[4];
    GlobalSignDistanceFieldPass::ConstantsData GlobalSDF;
    GlobalSurfaceAtlasPass::ConstantsData GlobalSurfaceAtlas;
    DynamicDiffuseGlobalIlluminationPass::ConstantsData DDGI;
    LightData Light;
    });

PACK_STRUCT(struct AtlasTileVertex
    {
    Half2 Position;
    Half2 TileUV;
    uint32 TileAddress;
    });

struct GlobalSurfaceAtlasTile : RectPack<GlobalSurfaceAtlasTile, uint16>
{
    Float3 ViewDirection;
    Float3 ViewPosition;
    Float3 ViewBoundsSize;
    Matrix ViewMatrix;
    uint32 Address;
    uint32 ObjectAddressOffset;

    GlobalSurfaceAtlasTile(uint16 x, uint16 y, uint16 width, uint16 height)
        : RectPack<GlobalSurfaceAtlasTile, uint16>(x, y, width, height)
    {
    }

    void OnInsert(class GlobalSurfaceAtlasCustomBuffer* buffer, void* actorObject, int32 tileIndex);

    void OnFree()
    {
    }
};

struct GlobalSurfaceAtlasObject
{
    uint64 LastFrameUsed;
    uint64 LastFrameDirty;
    Actor* Actor;
    GlobalSurfaceAtlasTile* Tiles[6];
    float Radius;
    OrientedBoundingBox Bounds;

    GlobalSurfaceAtlasObject()
    {
        Platform::MemoryClear(this, sizeof(GlobalSurfaceAtlasObject));
    }

    GlobalSurfaceAtlasObject(const GlobalSurfaceAtlasObject& other)
    {
        Platform::MemoryCopy(this, &other, sizeof(GlobalSurfaceAtlasObject));
    }

    GlobalSurfaceAtlasObject(GlobalSurfaceAtlasObject&& other) noexcept
    {
        Platform::MemoryCopy(this, &other, sizeof(GlobalSurfaceAtlasObject));
    }

    GlobalSurfaceAtlasObject& operator=(const GlobalSurfaceAtlasObject& other)
    {
        Platform::MemoryCopy(this, &other, sizeof(GlobalSurfaceAtlasObject));
        return *this;
    }

    GlobalSurfaceAtlasObject& operator=(GlobalSurfaceAtlasObject&& other) noexcept
    {
        Platform::MemoryCopy(this, &other, sizeof(GlobalSurfaceAtlasObject));
        return *this;
    }
};

class GlobalSurfaceAtlasCustomBuffer : public RenderBuffers::CustomBuffer, public ISceneRenderingListener
{
public:
    int32 Resolution = 0;
    uint64 LastFrameAtlasInsertFail = 0;
    uint64 LastFrameAtlasDefragmentation = 0;
    GPUTexture* AtlasDepth = nullptr;
    GPUTexture* AtlasEmissive = nullptr;
    GPUTexture* AtlasGBuffer0 = nullptr;
    GPUTexture* AtlasGBuffer1 = nullptr;
    GPUTexture* AtlasGBuffer2 = nullptr;
    GPUTexture* AtlasLighting = nullptr;
    GPUBuffer* ChunksBuffer = nullptr;
    GPUBuffer* CulledObjectsBuffer = nullptr;
    int32 CulledObjectsCounterIndex = -1;
    GlobalSurfaceAtlasPass::BindingData Result;
    GlobalSurfaceAtlasTile* AtlasTiles = nullptr; // TODO: optimize with a single allocation for atlas tiles
    Dictionary<void*, GlobalSurfaceAtlasObject> Objects;

    // Cached data to be reused during RasterizeActor
    uint64 CurrentFrame;
    float ResolutionInv;
    Float3 ViewPosition;
    float TileTexelsPerWorldUnit;
    float DistanceScalingStart;
    float DistanceScalingEnd;
    float DistanceScaling;

    FORCE_INLINE void ClearObjects()
    {
        CulledObjectsCounterIndex = -1;
        LastFrameAtlasDefragmentation = Engine::FrameCount;
        SAFE_DELETE(AtlasTiles);
        Objects.Clear();
    }

    FORCE_INLINE void Clear()
    {
        RenderTargetPool::Release(AtlasDepth);
        RenderTargetPool::Release(AtlasEmissive);
        RenderTargetPool::Release(AtlasGBuffer0);
        RenderTargetPool::Release(AtlasGBuffer1);
        RenderTargetPool::Release(AtlasGBuffer2);
        RenderTargetPool::Release(AtlasLighting);
        ClearObjects();
    }

    ~GlobalSurfaceAtlasCustomBuffer()
    {
        SAFE_DELETE_GPU_RESOURCE(ChunksBuffer);
        SAFE_DELETE_GPU_RESOURCE(CulledObjectsBuffer);
        Clear();
    }

    // [ISceneRenderingListener]
    void OnSceneRenderingAddActor(Actor* a) override
    {
    }

    void OnSceneRenderingUpdateActor(Actor* a, const BoundingSphere& prevBounds) override
    {
        // Dirty static objects to redraw when changed (eg. material modification)
        if (a->HasStaticFlag(StaticFlags::Lightmap))
        {
            GlobalSurfaceAtlasObject* object = Objects.TryGet(a);
            if (object)
            {
                // Dirty object to redraw
                object->LastFrameDirty = 0;
            }
        }
    }

    void OnSceneRenderingRemoveActor(Actor* a) override
    {
    }

    void OnSceneRenderingClear(SceneRendering* scene) override
    {
    }
};

void GlobalSurfaceAtlasTile::OnInsert(GlobalSurfaceAtlasCustomBuffer* buffer, void* actorObject, int32 tileIndex)
{
    buffer->Objects[actorObject].Tiles[tileIndex] = this;
}

String GlobalSurfaceAtlasPass::ToString() const
{
    return TEXT("GlobalSurfaceAtlasPass");
}

bool GlobalSurfaceAtlasPass::Init()
{
    // Check platform support
    const auto device = GPUDevice::Instance;
    _supported = device->GetFeatureLevel() >= FeatureLevel::SM5 && device->Limits.HasCompute && device->Limits.HasTypedUAVLoad;
    return false;
}

bool GlobalSurfaceAtlasPass::setupResources()
{
    if (!_supported)
        return true;

    // Load shader
    if (!_shader)
    {
        _shader = Content::LoadAsyncInternal<Shader>(TEXT("Shaders/GI/GlobalSurfaceAtlas"));
        if (_shader == nullptr)
            return true;
#if COMPILE_WITH_DEV_ENV
        _shader.Get()->OnReloading.Bind<GlobalSurfaceAtlasPass, &GlobalSurfaceAtlasPass::OnShaderReloading>(this);
#endif
    }
    if (!_shader->IsLoaded())
        return true;

    const auto device = GPUDevice::Instance;
    const auto shader = _shader->GetShader();
    _cb0 = shader->GetCB(0);
    if (!_cb0)
        return true;
    _csCullObjects = shader->GetCS("CS_CullObjects");

    // Create pipeline state
    GPUPipelineState::Description psDesc = GPUPipelineState::Description::DefaultFullscreenTriangle;
    if (!_psDebug)
    {
        _psDebug = device->CreatePipelineState();
        psDesc.PS = shader->GetPS("PS_Debug");
        if (_psDebug->Init(psDesc))
            return true;
    }
    if (!_psClear)
    {
        _psClear = device->CreatePipelineState();
        psDesc.DepthTestEnable = true;
        psDesc.DepthWriteEnable = true;
        psDesc.DepthFunc = ComparisonFunc::Always;
        psDesc.VS = shader->GetVS("VS_Atlas");
        psDesc.PS = shader->GetPS("PS_Clear");
        if (_psClear->Init(psDesc))
            return true;
    }
    if (!_psDirectLighting0)
    {
        _psDirectLighting0 = device->CreatePipelineState();
        psDesc.DepthTestEnable = false;
        psDesc.DepthWriteEnable = false;
        psDesc.DepthFunc = ComparisonFunc::Never;
        psDesc.BlendMode = BlendingMode::Add;
        psDesc.BlendMode.RenderTargetWriteMask = BlendingMode::ColorWrite::RGB;
        psDesc.PS = shader->GetPS("PS_Lighting", 0);
        if (_psDirectLighting0->Init(psDesc))
            return true;
        _psDirectLighting1 = device->CreatePipelineState();
        psDesc.PS = shader->GetPS("PS_Lighting", 1);
        if (_psDirectLighting1->Init(psDesc))
            return true;
        _psIndirectLighting = device->CreatePipelineState();
        psDesc.PS = shader->GetPS("PS_Lighting", 2);
        if (_psIndirectLighting->Init(psDesc))
            return true;
    }

    return false;
}

#if COMPILE_WITH_DEV_ENV

void GlobalSurfaceAtlasPass::OnShaderReloading(Asset* obj)
{
    SAFE_DELETE_GPU_RESOURCE(_psClear);
    SAFE_DELETE_GPU_RESOURCE(_psDirectLighting0);
    SAFE_DELETE_GPU_RESOURCE(_psDirectLighting1);
    SAFE_DELETE_GPU_RESOURCE(_psIndirectLighting);
    SAFE_DELETE_GPU_RESOURCE(_psDebug);
    invalidateResources();
}

#endif

void GlobalSurfaceAtlasPass::Dispose()
{
    RendererPass::Dispose();

    // Cleanup
    SAFE_DELETE(_vertexBuffer);
    SAFE_DELETE(_objectsBuffer);
    SAFE_DELETE_GPU_RESOURCE(_culledObjectsSizeBuffer);
    SAFE_DELETE_GPU_RESOURCE(_psClear);
    SAFE_DELETE_GPU_RESOURCE(_psDirectLighting0);
    SAFE_DELETE_GPU_RESOURCE(_psDirectLighting1);
    SAFE_DELETE_GPU_RESOURCE(_psIndirectLighting);
    SAFE_DELETE_GPU_RESOURCE(_psDebug);
    _cb0 = nullptr;
    _shader = nullptr;
}

bool GlobalSurfaceAtlasPass::Render(RenderContext& renderContext, GPUContext* context, BindingData& result)
{
    // Skip if not supported
    if (checkIfSkipPass())
        return true;
    if (renderContext.List->Scenes.Count() == 0)
        return true;
    auto& surfaceAtlasData = *renderContext.Buffers->GetCustomBuffer<GlobalSurfaceAtlasCustomBuffer>(TEXT("GlobalSurfaceAtlas"));

    // Render Global SDF (used for direct shadowing)
    GlobalSignDistanceFieldPass::BindingData bindingDataSDF;
    if (GlobalSignDistanceFieldPass::Instance()->Render(renderContext, context, bindingDataSDF))
        return true;

    // Skip if already done in the current frame
    const auto currentFrame = Engine::FrameCount;
    if (surfaceAtlasData.LastFrameUsed == currentFrame)
    {
        result = surfaceAtlasData.Result;
        return false;
    }
    surfaceAtlasData.LastFrameUsed = currentFrame;
    PROFILE_GPU_CPU("Global Surface Atlas");

    // Setup options
    const int32 resolution = Math::Clamp(Graphics::GlobalSurfaceAtlasResolution, 256, GPU_MAX_TEXTURE_SIZE);
    const float resolutionInv = 1.0f / resolution;
    auto& giSettings = renderContext.List->Settings.GlobalIllumination;
    const float distance = giSettings.Distance;

    // Initialize buffers
    bool noCache = surfaceAtlasData.Resolution != resolution;
    if (noCache)
    {
        surfaceAtlasData.Clear();

        auto desc = GPUTextureDescription::New2D(resolution, resolution, PixelFormat::Unknown);
        uint64 memUsage = 0;
        // TODO: try using BC4/BC5/BC7 block compression for Surface Atlas (eg. for Tiles material properties)
#define INIT_ATLAS_TEXTURE(texture, format) desc.Format = format; surfaceAtlasData.texture = RenderTargetPool::Get(desc); if (!surfaceAtlasData.texture) return true; memUsage += surfaceAtlasData.texture->GetMemoryUsage()
        INIT_ATLAS_TEXTURE(AtlasEmissive, LIGHT_BUFFER_FORMAT);
        INIT_ATLAS_TEXTURE(AtlasGBuffer0, GBUFFER0_FORMAT);
        INIT_ATLAS_TEXTURE(AtlasGBuffer1, GBUFFER1_FORMAT);
        INIT_ATLAS_TEXTURE(AtlasGBuffer2, GBUFFER2_FORMAT);
        INIT_ATLAS_TEXTURE(AtlasLighting, LIGHT_BUFFER_FORMAT);
        desc.Flags = GPUTextureFlags::DepthStencil | GPUTextureFlags::ShaderResource;
        INIT_ATLAS_TEXTURE(AtlasDepth, PixelFormat::D16_UNorm);
#undef INIT_ATLAS_TEXTURE
        surfaceAtlasData.Resolution = resolution;
        if (!surfaceAtlasData.ChunksBuffer)
        {
            surfaceAtlasData.ChunksBuffer = GPUDevice::Instance->CreateBuffer(TEXT("GlobalSurfaceAtlas.ChunksBuffer"));
            if (surfaceAtlasData.ChunksBuffer->Init(GPUBufferDescription::Raw(sizeof(uint32) * GLOBAL_SURFACE_ATLAS_CHUNKS_RESOLUTION * GLOBAL_SURFACE_ATLAS_CHUNKS_RESOLUTION * GLOBAL_SURFACE_ATLAS_CHUNKS_RESOLUTION, GPUBufferFlags::ShaderResource | GPUBufferFlags::UnorderedAccess)))
                return true;
            memUsage += surfaceAtlasData.ChunksBuffer->GetMemoryUsage();
        }
        LOG(Info, "Global Surface Atlas resolution: {0}, memory usage: {1} MB", resolution, memUsage / 1024 / 1024);
    }
    else
    {
        // Perform atlas defragmentation if needed
        // TODO: track atlas used vs free ratio to skip defragmentation if it's nearly full (then maybe auto resize up?)
        if (currentFrame - surfaceAtlasData.LastFrameAtlasInsertFail < 10 &&
            currentFrame - surfaceAtlasData.LastFrameAtlasDefragmentation > 60)
        {
            surfaceAtlasData.ClearObjects();
        }
    }
    for (SceneRendering* scene : renderContext.List->Scenes)
        surfaceAtlasData.ListenSceneRendering(scene);
    if (!surfaceAtlasData.AtlasTiles)
        surfaceAtlasData.AtlasTiles = New<GlobalSurfaceAtlasTile>(0, 0, resolution, resolution);
    if (!_vertexBuffer)
        _vertexBuffer = New<DynamicVertexBuffer>(0u, (uint32)sizeof(AtlasTileVertex), TEXT("GlobalSurfaceAtlas.VertexBuffer"));
    if (!_objectsBuffer)
        _objectsBuffer = New<DynamicTypedBuffer>(256 * (GLOBAL_SURFACE_ATLAS_OBJECT_DATA_STRIDE + GLOBAL_SURFACE_ATLAS_TILE_DATA_STRIDE * 3 / 4), PixelFormat::R32G32B32A32_Float, false, TEXT("GlobalSurfaceAtlas.ObjectsBuffer"));

    // Utility for writing into tiles vertex buffer
    const Float2 posToClipMul(2.0f * resolutionInv, -2.0f * resolutionInv);
    const Float2 posToClipAdd(-1.0f, 1.0f);
#define VB_WRITE_TILE_POS_ONLY(tile) \
        Float2 minPos((float)tile->X, (float)tile->Y), maxPos((float)(tile->X + tile->Width), (float)(tile->Y + tile->Height)); \
        Half2 min(minPos * posToClipMul + posToClipAdd), max(maxPos * posToClipMul + posToClipAdd); \
        auto* quad = _vertexBuffer->WriteReserve<AtlasTileVertex>(6); \
        quad[0].Position  = max; \
        quad[1].Position = { min.X, max.Y }; \
        quad[2].Position = min; \
        quad[3].Position = quad[2].Position; \
        quad[4].Position = { max.X, min.Y }; \
        quad[5].Position = quad[0].Position
#define VB_WRITE_TILE(tile) \
        Float2 minPos((float)tile->X, (float)tile->Y), maxPos((float)(tile->X + tile->Width), (float)(tile->Y + tile->Height)); \
        Half2 min(minPos * posToClipMul + posToClipAdd), max(maxPos * posToClipMul + posToClipAdd); \
        Float2 minUV(0, 0), maxUV(1, 1); \
        auto* quad = _vertexBuffer->WriteReserve<AtlasTileVertex>(6); \
        quad[0] = { { max }, { maxUV }, tile->Address }; \
        quad[1] = { { min.X, max.Y }, { minUV.X, maxUV.Y }, tile->Address }; \
        quad[2] = { { min }, { minUV }, tile->Address }; \
        quad[3] = quad[2]; \
        quad[4] = { { max.X, min.Y }, { maxUV.X, minUV.Y }, tile->Address }; \
        quad[5] = quad[0]
#define VB_DRAW() \
        _vertexBuffer->Flush(context); \
        auto vb = _vertexBuffer->GetBuffer(); \
        context->BindVB(ToSpan(&vb, 1)); \
        context->DrawInstanced(_vertexBuffer->Data.Count() / sizeof(AtlasTileVertex), 1);

    // Add objects into the atlas
    {
        PROFILE_CPU_NAMED("Draw");
        _objectsBuffer->Clear();
        _dirtyObjectsBuffer.Clear();
        _surfaceAtlasData = &surfaceAtlasData;
        renderContext.View.Pass = DrawPass::GlobalSurfaceAtlas;
        surfaceAtlasData.CurrentFrame = currentFrame;
        surfaceAtlasData.ResolutionInv = resolutionInv;
        surfaceAtlasData.ViewPosition = renderContext.View.Position;
        surfaceAtlasData.TileTexelsPerWorldUnit = 1.0f / 10.0f; // Scales the tiles resolution
        surfaceAtlasData.DistanceScalingStart = 2000.0f; // Distance from camera at which the tiles resolution starts to be scaled down
        surfaceAtlasData.DistanceScalingEnd = 5000.0f; // Distance from camera at which the tiles resolution end to be scaled down
        surfaceAtlasData.DistanceScaling = 0.2f; // The scale for tiles at distanceScalingEnd and further away
        // TODO: add DetailsScale param to adjust quality of scene details in Global Surface Atlas
        const uint32 viewMask = renderContext.View.RenderLayersMask;
        const Float3 viewPosition = renderContext.View.Position;
        const float minObjectRadius = 20.0f; // Skip too small objects
        for (auto* scene : renderContext.List->Scenes)
        {
            for (auto& e : scene->Actors)
            {
                if (viewMask & e.LayerMask && e.Bounds.Radius >= minObjectRadius && CollisionsHelper::DistanceSpherePoint(e.Bounds, viewPosition) < distance)
                {
                    e.Actor->Draw(renderContext);
                }
            }
        }
    }

    // Remove unused objects
    for (auto it = surfaceAtlasData.Objects.Begin(); it.IsNotEnd(); ++it)
    {
        if (it->Value.LastFrameUsed != currentFrame)
        {
            for (auto& tile : it->Value.Tiles)
            {
                if (tile)
                    tile->Free();
            }
            surfaceAtlasData.Objects.Remove(it);
        }
    }

    // Rasterize world geometry material properties into Global Surface Atlas
    if (_dirtyObjectsBuffer.Count() != 0)
    {
        PROFILE_GPU_CPU("Rasterize Tiles");

        RenderContext renderContextTiles = renderContext;
        renderContextTiles.List = RenderList::GetFromPool();
        renderContextTiles.View.Pass = DrawPass::GBuffer;
        renderContextTiles.View.Mode = ViewMode::Default;
        renderContextTiles.View.ModelLODBias += 100000;
        renderContextTiles.View.ShadowModelLODBias += 100000;
        renderContextTiles.View.IsSingleFrame = true;
        renderContextTiles.View.IsCullingDisabled = true;
        renderContextTiles.View.Near = 0.0f;
        renderContextTiles.View.Prepare(renderContextTiles);

        GPUTextureView* depthBuffer = surfaceAtlasData.AtlasDepth->View();
        GPUTextureView* targetBuffers[4] =
        {
            surfaceAtlasData.AtlasEmissive->View(),
            surfaceAtlasData.AtlasGBuffer0->View(),
            surfaceAtlasData.AtlasGBuffer1->View(),
            surfaceAtlasData.AtlasGBuffer2->View(),
        };
        context->SetRenderTarget(depthBuffer, ToSpan(targetBuffers, ARRAY_COUNT(targetBuffers)));
        {
            PROFILE_GPU_CPU("Clear");
            if (noCache || GLOBAL_SURFACE_ATLAS_DEBUG_FORCE_REDRAW_TILES)
            {
                // Full-atlas hardware clear
                context->ClearDepth(depthBuffer);
                context->Clear(targetBuffers[0], Color::Transparent);
                context->Clear(targetBuffers[1], Color::Transparent);
                context->Clear(targetBuffers[2], Color::Transparent);
                context->Clear(targetBuffers[3], Color(1, 0, 0, 0));
            }
            else
            {
                // Per-tile clear (with a single draw call)
                _vertexBuffer->Clear();
                _vertexBuffer->Data.EnsureCapacity(_dirtyObjectsBuffer.Count() * 6 * sizeof(AtlasTileVertex));
                for (void* actorObject : _dirtyObjectsBuffer)
                {
                    const auto& object = ((const Dictionary<Actor*, GlobalSurfaceAtlasObject>&)surfaceAtlasData.Objects)[actorObject];
                    for (int32 tileIndex = 0; tileIndex < 6; tileIndex++)
                    {
                        auto* tile = object.Tiles[tileIndex];
                        if (!tile)
                            continue;
                        VB_WRITE_TILE_POS_ONLY(tile);
                    }
                }
                context->SetState(_psClear);
                context->SetViewportAndScissors(Viewport(0, 0, (float)resolution, (float)resolution));
                VB_DRAW();
            }
        }
        // TODO: limit dirty objects count on a first frame (eg. collect overflown objects to be redirty next frame)
        auto& drawCallsListGBuffer = renderContextTiles.List->DrawCallsLists[(int32)DrawCallsListType::GBuffer];
        auto& drawCallsListGBufferNoDecals = renderContextTiles.List->DrawCallsLists[(int32)DrawCallsListType::GBufferNoDecals];
        drawCallsListGBuffer.CanUseInstancing = false;
        drawCallsListGBufferNoDecals.CanUseInstancing = false;
        for (void* actorObject : _dirtyObjectsBuffer)
        {
            const auto& object = ((const Dictionary<Actor*, GlobalSurfaceAtlasObject>&)surfaceAtlasData.Objects)[actorObject];

            // Clear draw calls list
            renderContextTiles.List->DrawCalls.Clear();
            renderContextTiles.List->BatchedDrawCalls.Clear();
            drawCallsListGBuffer.Indices.Clear();
            drawCallsListGBuffer.PreBatchedDrawCalls.Clear();
            drawCallsListGBufferNoDecals.Indices.Clear();
            drawCallsListGBufferNoDecals.PreBatchedDrawCalls.Clear();

            // Fake projection matrix to disable Screen Size culling based on RenderTools::ComputeBoundsScreenRadiusSquared
            renderContextTiles.View.Projection.Values[0][0] = 10000.0f;

            // Collect draw calls for the object
            object.Actor->Draw(renderContextTiles);

            // Render all tiles into the atlas
#if GLOBAL_SURFACE_ATLAS_DEBUG_DRAW_OBJECTS
            DebugDraw::DrawBox(object.Bounds, Color::Red.AlphaMultiplied(0.4f));
#endif
            for (int32 tileIndex = 0; tileIndex < 6; tileIndex++)
            {
                auto* tile = object.Tiles[tileIndex];
                if (!tile)
                    continue;
                const float tileWidth = (float)tile->Width - GLOBAL_SURFACE_ATLAS_TILE_PADDING;
                const float tileHeight = (float)tile->Height - GLOBAL_SURFACE_ATLAS_TILE_PADDING;

                // Setup projection to capture object from the side
                renderContextTiles.View.Position = tile->ViewPosition;
                renderContextTiles.View.Direction = tile->ViewDirection;
                renderContextTiles.View.Near = -GLOBAL_SURFACE_ATLAS_TILE_PROJ_PLANE_OFFSET;
                renderContextTiles.View.Far = tile->ViewBoundsSize.Z + 2 * GLOBAL_SURFACE_ATLAS_TILE_PROJ_PLANE_OFFSET;
                Matrix projectionMatrix;
                Matrix::Ortho(tile->ViewBoundsSize.X, tile->ViewBoundsSize.Y, renderContextTiles.View.Near, renderContextTiles.View.Far, projectionMatrix);
                renderContextTiles.View.SetUp(tile->ViewMatrix, projectionMatrix);
#if GLOBAL_SURFACE_ATLAS_DEBUG_DRAW_OBJECTS
                DebugDraw::DrawLine(renderContextTiles.View.Position, renderContextTiles.View.Position + renderContextTiles.View.Direction * 20.0f, Color::Orange);
                DebugDraw::DrawWireSphere(BoundingSphere(renderContextTiles.View.Position, 10.0f), Color::Green);
#endif

                // Draw
                context->SetViewportAndScissors(Viewport(tile->X, tile->Y, tileWidth, tileHeight));
                renderContextTiles.List->ExecuteDrawCalls(renderContextTiles, drawCallsListGBuffer);
                renderContextTiles.List->ExecuteDrawCalls(renderContextTiles, drawCallsListGBufferNoDecals);
            }
        }
        context->ResetRenderTarget();
        RenderList::ReturnToPool(renderContextTiles.List);
    }

    // Send objects data to the GPU
    {
        PROFILE_GPU_CPU("Update Objects");
        _objectsBuffer->Flush(context);
    }

    // Init constants
    result.Constants.ViewPos = renderContext.View.Position;
    result.Constants.Resolution = (float)resolution;
    result.Constants.ChunkSize = distance / (float)GLOBAL_SURFACE_ATLAS_CHUNKS_RESOLUTION;
    result.Constants.ObjectsCount = surfaceAtlasData.Objects.Count();

    // If we don't know the culled objects buffer capacity then we shouldn't use atlas results as many objects are still missing (see CulledObjectsCounterIndex usage)
    bool notReady = false;

    // Cull objects into chunks (for faster Atlas sampling)
    if (surfaceAtlasData.Objects.Count() != 0)
    {
        // Each chunk (ChunksBuffer) contains uint with address of the culled objects data start in CulledObjectsBuffer.
        // If chunk has address=0 then it's unused/empty.
        // Chunk [0,0,0] is unused and it's address=0 is used for atomic counter for writing into CulledObjectsBuffer.
        // Each chunk data contains objects count + all objects with tiles copied into buffer.
        // This allows to quickly convert world-space position into chunk, then read chunk data start and loop over culled objects (less objects and data already in place).
        PROFILE_GPU_CPU("Cull Objects");
        uint32 objectsBufferCapacity = (uint32)((float)_objectsBuffer->Data.Count() * 1.3f);

        // Copy counter from ChunksBuffer into staging buffer to access current chunks memory usage to adapt dynamically to the scene complexity
        if (surfaceAtlasData.ChunksBuffer)
        {
            if (!_culledObjectsSizeBuffer)
            {
                Platform::MemoryClear(_culledObjectsSizeFrames, sizeof(_culledObjectsSizeFrames));
                _culledObjectsSizeBuffer = GPUDevice::Instance->CreateBuffer(TEXT("GlobalSurfaceAtlas.CulledObjectsSizeBuffer"));
                const GPUBufferDescription desc = GPUBufferDescription::Buffer(ARRAY_COUNT(_culledObjectsSizeFrames) * sizeof(uint32), GPUBufferFlags::None, PixelFormat::R32_UInt, _culledObjectsSizeFrames, sizeof(uint32), GPUResourceUsage::StagingReadback);
                if (_culledObjectsSizeBuffer->Init(desc))
                    return true;
            }
            if (surfaceAtlasData.CulledObjectsCounterIndex != -1)
            {
                // Get the last counter value (accept staging readback delay)
                notReady = true;
                auto data = (uint32*)_culledObjectsSizeBuffer->Map(GPUResourceMapMode::Read);
                if (data)
                {
                    uint32 counter = data[surfaceAtlasData.CulledObjectsCounterIndex];
                    _culledObjectsSizeBuffer->Unmap();
                    if (counter > 0)
                    {
                        objectsBufferCapacity = counter * sizeof(Float4);
                        notReady = false;
                    }
                }
            }
            if (surfaceAtlasData.CulledObjectsCounterIndex == -1)
            {
                // Find a free timer slot
                notReady = true;
                for (int32 i = 0; i < ARRAY_COUNT(_culledObjectsSizeFrames); i++)
                {
                    if (currentFrame - _culledObjectsSizeFrames[i] > GPU_ASYNC_LATENCY)
                    {
                        surfaceAtlasData.CulledObjectsCounterIndex = i;
                        break;
                    }
                }
            }
            if (surfaceAtlasData.CulledObjectsCounterIndex != -1)
            {
                // Copy current counter value
                _culledObjectsSizeFrames[surfaceAtlasData.CulledObjectsCounterIndex] = currentFrame;
                context->CopyBuffer(_culledObjectsSizeBuffer, surfaceAtlasData.ChunksBuffer, sizeof(uint32), surfaceAtlasData.CulledObjectsCounterIndex * sizeof(uint32), 0);
            }
        }

        // Allocate buffer for culled objects (estimated size)
        objectsBufferCapacity = Math::Min(Math::AlignUp(objectsBufferCapacity, 4096u), (uint32)MAX_int32);
        if (!surfaceAtlasData.CulledObjectsBuffer)
            surfaceAtlasData.CulledObjectsBuffer = GPUDevice::Instance->CreateBuffer(TEXT("GlobalSurfaceAtlas.CulledObjectsBuffer"));
        if (surfaceAtlasData.CulledObjectsBuffer->GetSize() < objectsBufferCapacity)
        {
            const GPUBufferDescription desc = GPUBufferDescription::Buffer(objectsBufferCapacity, GPUBufferFlags::UnorderedAccess | GPUBufferFlags::ShaderResource, PixelFormat::R32G32B32A32_Float, nullptr, sizeof(Float4));
            if (surfaceAtlasData.CulledObjectsBuffer->Init(desc))
                return true;
        }

        // Clear chunks counter (chunk at 0 is used for a counter so chunks buffer is aligned)
        uint32 counter = 1; // Indicate that 1st float4 is used so value 0 can be used as invalid chunk address
        context->UpdateBuffer(surfaceAtlasData.ChunksBuffer, &counter, sizeof(counter), 0);

        // Cull objects into chunks (1 thread per chunk)
        Data0 data;
        data.ViewWorldPos = renderContext.View.Position;
        data.ViewNearPlane = renderContext.View.Near;
        data.ViewFarPlane = renderContext.View.Far;
        data.CulledObjectsCapacity = objectsBufferCapacity;
        data.GlobalSurfaceAtlas = result.Constants;
        context->UpdateCB(_cb0, &data);
        context->BindCB(0, _cb0);
        static_assert(GLOBAL_SURFACE_ATLAS_CHUNKS_RESOLUTION % GLOBAL_SURFACE_ATLAS_CHUNKS_GROUP_SIZE == 0, "Invalid chunks resolution/groups setting.");
        const int32 chunkDispatchGroups = GLOBAL_SURFACE_ATLAS_CHUNKS_RESOLUTION / GLOBAL_SURFACE_ATLAS_CHUNKS_GROUP_SIZE;
        context->BindSR(0, _objectsBuffer->GetBuffer()->View());
        context->BindUA(0, surfaceAtlasData.ChunksBuffer->View());
        context->BindUA(1, surfaceAtlasData.CulledObjectsBuffer->View());
        context->Dispatch(_csCullObjects, chunkDispatchGroups, chunkDispatchGroups, chunkDispatchGroups);
        context->ResetUA();

#if GLOBAL_SURFACE_ATLAS_DEBUG_DRAW_CHUNKS
        // Debug draw tiles that have any objects inside
        for (int32 z = 0; z < GLOBAL_SURFACE_ATLAS_CHUNKS_RESOLUTION; z++)
        {
            for (int32 y = 0; y < GLOBAL_SURFACE_ATLAS_CHUNKS_RESOLUTION; y++)
            {
                for (int32 x = 0; x < GLOBAL_SURFACE_ATLAS_CHUNKS_RESOLUTION; x++)
                {
                    Float3 chunkCoord(x, y, z);
                    Float3 chunkMin = result.GlobalSurfaceAtlas.ViewPos + (chunkCoord - (GLOBAL_SURFACE_ATLAS_CHUNKS_RESOLUTION * 0.5f)) * result.GlobalSurfaceAtlas.ChunkSize;
                    Float3 chunkMax = chunkMin + result.GlobalSurfaceAtlas.ChunkSize;
                    BoundingBox chunkBounds(chunkMin, chunkMax);
                    if (Float3::Distance(chunkBounds.GetCenter(), result.GlobalSurfaceAtlas.ViewPos) >= 2000.0f)
                        continue;

                    int32 count = 0;
                    for (auto& e : surfaceAtlasData.Objects)
                    {
                        BoundingSphere objectBounds(e.Value.Bounds.GetCenter(), e.Value.Radius);
                        if (chunkBounds.Intersects(objectBounds))
                            count++;
                    }
                    if (count != 0)
                    {
                        DebugDraw::DrawText(String::Format(TEXT("{} Objects"), count), chunkBounds.GetCenter(), Color::Green);
                        DebugDraw::DrawWireBox(chunkBounds, Color::Green);
                    }
                }
            }
        }
#endif
    }

    // Copy results
    result.Atlas[0] = surfaceAtlasData.AtlasDepth;
    result.Atlas[1] = surfaceAtlasData.AtlasGBuffer0;
    result.Atlas[2] = surfaceAtlasData.AtlasGBuffer1;
    result.Atlas[3] = surfaceAtlasData.AtlasGBuffer2;
    result.Atlas[4] = surfaceAtlasData.AtlasLighting;
    result.Chunks = surfaceAtlasData.ChunksBuffer;
    result.CulledObjects = surfaceAtlasData.CulledObjectsBuffer;
    surfaceAtlasData.Result = result;

    // Render direct lighting into atlas
    if (surfaceAtlasData.Objects.Count() != 0)
    {
        PROFILE_GPU_CPU("Direct Lighting");

        // Copy emissive light into the final direct lighting atlas
        // TODO: test perf diff when manually copying only dirty object tiles and dirty light tiles together with indirect lighting
        {
            PROFILE_GPU_CPU("Copy Emissive");
            context->CopyTexture(surfaceAtlasData.AtlasLighting, 0, 0, 0, 0, surfaceAtlasData.AtlasEmissive, 0);
        }

        context->SetViewportAndScissors(Viewport(0, 0, (float)resolution, (float)resolution));
        context->SetRenderTarget(surfaceAtlasData.AtlasLighting->View());
        context->BindSR(0, surfaceAtlasData.AtlasGBuffer0->View());
        context->BindSR(1, surfaceAtlasData.AtlasGBuffer1->View());
        context->BindSR(2, surfaceAtlasData.AtlasGBuffer2->View());
        context->BindSR(3, surfaceAtlasData.AtlasDepth->View());
        context->BindSR(4, _objectsBuffer->GetBuffer()->View());
        context->BindSR(5, bindingDataSDF.Texture ? bindingDataSDF.Texture->ViewVolume() : nullptr);
        context->BindSR(6, bindingDataSDF.TextureMip ? bindingDataSDF.TextureMip->ViewVolume() : nullptr);
        context->BindCB(0, _cb0);
        Data0 data;
        data.ViewWorldPos = renderContext.View.Position;
        data.GlobalSDF = bindingDataSDF.Constants;
        data.GlobalSurfaceAtlas = result.Constants;

        // Shade object tiles influenced by lights to calculate direct lighting
        // TODO: reduce redraw frequency for static lights (StaticFlags::Lightmap)
        for (auto& light : renderContext.List->DirectionalLights)
        {
            // Collect tiles to shade
            _vertexBuffer->Clear();
            for (const auto& e : surfaceAtlasData.Objects)
            {
                const auto& object = e.Value;
                for (int32 tileIndex = 0; tileIndex < 6; tileIndex++)
                {
                    auto* tile = object.Tiles[tileIndex];
                    if (!tile || Float3::Dot(tile->ViewDirection, light.Direction) < ZeroTolerance)
                        continue;
                    VB_WRITE_TILE(tile);
                }
            }

            // Draw draw light
            const bool useShadow = CanRenderShadow(renderContext.View, light);
            // TODO: test perf/quality when using Shadow Map for directional light (ShadowsPass::Instance()->LastDirLightShadowMap) instead of Global SDF trace
            light.SetupLightData(&data.Light, useShadow);
            data.Light.Color *= light.IndirectLightingIntensity;
            data.LightShadowsStrength = 1.0f - light.ShadowsStrength;
            context->UpdateCB(_cb0, &data);
            context->SetState(_psDirectLighting0);
            VB_DRAW();
        }
        for (auto& light : renderContext.List->PointLights)
        {
            // Collect tiles to shade
            _vertexBuffer->Clear();
            for (const auto& e : surfaceAtlasData.Objects)
            {
                const auto& object = e.Value;
                Float3 lightToObject = object.Bounds.GetCenter() - light.Position;
                if (lightToObject.LengthSquared() >= Math::Square(object.Radius + light.Radius))
                    continue;
                for (int32 tileIndex = 0; tileIndex < 6; tileIndex++)
                {
                    auto* tile = object.Tiles[tileIndex];
                    if (!tile)
                        continue;
                    VB_WRITE_TILE(tile);
                }
            }

            // Draw draw light
            const bool useShadow = CanRenderShadow(renderContext.View, light);
            light.SetupLightData(&data.Light, useShadow);
            data.Light.Color *= light.IndirectLightingIntensity;
            data.LightShadowsStrength = 1.0f - light.ShadowsStrength;
            context->UpdateCB(_cb0, &data);
            context->SetState(_psDirectLighting1);
            VB_DRAW();
        }
        for (auto& light : renderContext.List->SpotLights)
        {
            // Collect tiles to shade
            _vertexBuffer->Clear();
            for (const auto& e : surfaceAtlasData.Objects)
            {
                const auto& object = e.Value;
                Float3 lightToObject = object.Bounds.GetCenter() - light.Position;
                if (lightToObject.LengthSquared() >= Math::Square(object.Radius + light.Radius))
                    continue;
                for (int32 tileIndex = 0; tileIndex < 6; tileIndex++)
                {
                    auto* tile = object.Tiles[tileIndex];
                    if (!tile || Float3::Dot(tile->ViewDirection, light.Direction) < ZeroTolerance)
                        continue;
                    VB_WRITE_TILE(tile);
                }
            }

            // Draw draw light
            const bool useShadow = CanRenderShadow(renderContext.View, light);
            light.SetupLightData(&data.Light, useShadow);
            data.Light.Color *= light.IndirectLightingIntensity;
            data.LightShadowsStrength = 1.0f - light.ShadowsStrength;
            context->UpdateCB(_cb0, &data);
            context->SetState(_psDirectLighting1);
            VB_DRAW();
        }
        if (renderContext.View.Flags & ViewFlags::GI)
        {
            // Draw draw indirect light from Global Illumination
            switch (renderContext.List->Settings.GlobalIllumination.Mode)
            {
            case GlobalIlluminationMode::DDGI:
            {
                DynamicDiffuseGlobalIlluminationPass::BindingData bindingDataDDGI;
                if (!DynamicDiffuseGlobalIlluminationPass::Instance()->Get(renderContext.Buffers, bindingDataDDGI))
                {
                    _vertexBuffer->Clear();
                    for (const auto& e : surfaceAtlasData.Objects)
                    {
                        const auto& object = e.Value;
                        for (int32 tileIndex = 0; tileIndex < 6; tileIndex++)
                        {
                            auto* tile = object.Tiles[tileIndex];
                            if (!tile)
                                continue;
                            VB_WRITE_TILE(tile);
                        }
                    }
                    data.DDGI = bindingDataDDGI.Constants;
                    context->BindSR(5, bindingDataDDGI.ProbesState);
                    context->BindSR(6, bindingDataDDGI.ProbesDistance);
                    context->BindSR(7, bindingDataDDGI.ProbesIrradiance);
                    context->UpdateCB(_cb0, &data);
                    context->SetState(_psIndirectLighting);
                    VB_DRAW();
                }
                break;
            }
            }
        }

        context->ResetSR();
        context->ResetRenderTarget();
    }

    // TODO: explore atlas tiles optimization with feedback from renderer (eg. when tile is sampled by GI/Reflections mark it as used, then sort tiles by importance and prioritize updates for ones frequently used)

#undef WRITE_TILE
    return notReady;
}

void GlobalSurfaceAtlasPass::RenderDebug(RenderContext& renderContext, GPUContext* context, GPUTexture* output)
{
    // Render all dependant effects before
    if (renderContext.View.Flags & ViewFlags::GI)
    {
        switch (renderContext.List->Settings.GlobalIllumination.Mode)
        {
        case GlobalIlluminationMode::DDGI:
            DynamicDiffuseGlobalIlluminationPass::Instance()->Render(renderContext, context, nullptr);
            break;
        }
    }
    GlobalSignDistanceFieldPass::BindingData bindingDataSDF;
    BindingData bindingData;
    if (GlobalSignDistanceFieldPass::Instance()->Render(renderContext, context, bindingDataSDF) || Render(renderContext, context, bindingData))
    {
        context->Draw(output, renderContext.Buffers->GBuffer0);
        return;
    }
    GPUTextureView* skybox = GBufferPass::Instance()->RenderSkybox(renderContext, context);

    PROFILE_GPU_CPU("Global Surface Atlas Debug");
    const Float2 outputSize(output->Size());
    Data0 data;
    {
        data.ViewWorldPos = renderContext.View.Position;
        data.ViewNearPlane = renderContext.View.Near;
        data.ViewFarPlane = renderContext.View.Far;
        for (int32 i = 0; i < 4; i++)
            data.ViewFrustumWorldRays[i] = Float4(renderContext.List->FrustumCornersWs[i + 4], 0);
        data.GlobalSDF = bindingDataSDF.Constants;
        data.GlobalSurfaceAtlas = bindingData.Constants;
        data.SkyboxIntensity = 1.0f;
        context->UpdateCB(_cb0, &data);
        context->BindCB(0, _cb0);
    }
    context->BindSR(0, bindingDataSDF.Texture ? bindingDataSDF.Texture->ViewVolume() : nullptr);
    context->BindSR(1, bindingDataSDF.TextureMip ? bindingDataSDF.TextureMip->ViewVolume() : nullptr);
    context->BindSR(2, bindingData.Chunks ? bindingData.Chunks->View() : nullptr);
    context->BindSR(3, bindingData.CulledObjects ? bindingData.CulledObjects->View() : nullptr);
    context->BindSR(4, bindingData.AtlasDepth->View());
    context->BindSR(6, skybox);
    context->SetState(_psDebug);
    {
        Float2 outputSizeThird = outputSize * 0.333f;
        Float2 outputSizeTwoThird = outputSize * 0.666f;

        GPUTexture* tempBuffer = renderContext.Buffers->RT2_FloatRGB;
        context->Clear(tempBuffer->View(), Color::Black);
        context->SetRenderTarget(tempBuffer->View());

        // Full screen - direct light
        context->BindSR(5, bindingData.AtlasLighting->View());
        context->SetViewport(outputSize.X, outputSize.Y);
        context->SetScissor(Rectangle(0, 0, outputSizeTwoThird.X, outputSize.Y));
        context->DrawFullscreenTriangle();

        // Color Grading and Post-Processing to improve readability in bright/dark scenes
        context->ResetRenderTarget();
        auto colorGradingLUT = ColorGradingPass::Instance()->RenderLUT(renderContext);
        EyeAdaptationPass::Instance()->Render(renderContext, tempBuffer);
        PostProcessingPass::Instance()->Render(renderContext, tempBuffer, output, colorGradingLUT);
        RenderTargetPool::Release(colorGradingLUT);
        context->ResetRenderTarget();

        // Rebind resources
        context->BindSR(0, bindingDataSDF.Texture ? bindingDataSDF.Texture->ViewVolume() : nullptr);
        context->BindSR(1, bindingDataSDF.TextureMip ? bindingDataSDF.TextureMip->ViewVolume() : nullptr);
        context->BindSR(2, bindingData.Chunks ? bindingData.Chunks->View() : nullptr);
        context->BindSR(3, bindingData.CulledObjects ? bindingData.CulledObjects->View() : nullptr);
        context->BindSR(4, bindingData.AtlasDepth->View());
        context->BindSR(6, skybox);
        context->BindCB(0, _cb0);
        context->SetState(_psDebug);
        context->SetRenderTarget(output->View());

        // Disable skybox
        data.SkyboxIntensity = 0.0f;
        context->UpdateCB(_cb0, &data);

        // Bottom left - diffuse
        context->BindSR(5, bindingData.AtlasGBuffer0->View());
        context->SetViewportAndScissors(Viewport(outputSizeTwoThird.X, 0, outputSizeThird.X, outputSizeThird.Y));
        context->DrawFullscreenTriangle();

        // Bottom middle - normals
        context->BindSR(5, bindingData.AtlasGBuffer1->View());
        context->SetViewportAndScissors(Viewport(outputSizeTwoThird.X, outputSizeThird.Y, outputSizeThird.X, outputSizeThird.Y));
        context->DrawFullscreenTriangle();

        // Bottom right - roughness/metalness/ao
        context->BindSR(5, bindingData.AtlasGBuffer2->View());
        context->SetViewportAndScissors(Viewport(outputSizeTwoThird.X, outputSizeTwoThird.Y, outputSizeThird.X, outputSizeThird.Y));
        context->DrawFullscreenTriangle();
    }
}

void GlobalSurfaceAtlasPass::RasterizeActor(Actor* actor, void* actorObject, const BoundingSphere& actorObjectBounds, const Matrix& localToWorld, const BoundingBox& localBounds, uint32 tilesMask)
{
    GlobalSurfaceAtlasCustomBuffer& surfaceAtlasData = *_surfaceAtlasData;
    Float3 boundsSize = localBounds.GetSize() * actor->GetScale();
    const float distanceScale = Math::Lerp(1.0f, surfaceAtlasData.DistanceScaling, Math::InverseLerp(surfaceAtlasData.DistanceScalingStart, surfaceAtlasData.DistanceScalingEnd, (float)CollisionsHelper::DistanceSpherePoint(actorObjectBounds, surfaceAtlasData.ViewPosition)));
    const float tilesScale = surfaceAtlasData.TileTexelsPerWorldUnit * distanceScale;
    GlobalSurfaceAtlasObject* object = surfaceAtlasData.Objects.TryGet(actorObject);
    bool anyTile = false, dirty = false;
    for (int32 tileIndex = 0; tileIndex < 6; tileIndex++)
    {
        if (((1 << tileIndex) & tilesMask) == 0)
            continue;

        // Calculate optimal tile resolution for the object side
        Float3 boundsSizeTile = boundsSize;
        boundsSizeTile.Raw[tileIndex / 2] = MAX_float; // Ignore depth size
        uint16 tileResolution = (uint16)(boundsSizeTile.GetAbsolute().MinValue() * tilesScale);
        if (tileResolution < 4)
        {
            // Skip too small surfaces
            if (object && object->Tiles[tileIndex])
            {
                object->Tiles[tileIndex]->Free();
                object->Tiles[tileIndex] = nullptr;
            }
            continue;
        }

        // Clamp tile resolution (in pixels)
        static_assert(GLOBAL_SURFACE_ATLAS_TILE_PADDING < GLOBAL_SURFACE_ATLAS_TILE_SIZE_MIN, "Invalid tile size configuration. Minimum tile size must be larger than padding.");
        tileResolution = Math::Clamp<uint16>(tileResolution, GLOBAL_SURFACE_ATLAS_TILE_SIZE_MIN, GLOBAL_SURFACE_ATLAS_TILE_SIZE_MAX);

        // Snap tiles resolution (down) which allows to reuse atlas slots once object gets resizes/replaced by other object
        tileResolution = Math::AlignDown<uint16>(tileResolution, 8);

        // Reuse current tile (refit only on a significant resolution change)
        if (object && object->Tiles[tileIndex])
        {
            const uint16 tileRefitResolutionStep = 32;
            const uint16 currentSize = object->Tiles[tileIndex]->Width;
            if (Math::Abs(tileResolution - currentSize) < tileRefitResolutionStep)
            {
                anyTile = true;
                continue;
            }
            object->Tiles[tileIndex]->Free();
        }

        // Insert tile into atlas
        auto* tile = surfaceAtlasData.AtlasTiles->Insert(tileResolution, tileResolution, 0, &surfaceAtlasData, actorObject, tileIndex);
        if (tile)
        {
            if (!object)
                object = &surfaceAtlasData.Objects[actorObject];
            object->Tiles[tileIndex] = tile;
            anyTile = true;
            dirty = true;
        }
        else
        {
            if (object)
                object->Tiles[tileIndex] = nullptr;
            surfaceAtlasData.LastFrameAtlasInsertFail = surfaceAtlasData.CurrentFrame;
        }
    }
    if (!anyTile)
        return;

    // Redraw objects from time-to-time (dynamic objects can be animated, static objects can have textures streamed)
    uint32 redrawFramesCount = actor->HasStaticFlag(StaticFlags::Lightmap) ? 120 : 4;
    if (surfaceAtlasData.CurrentFrame - object->LastFrameDirty >= (redrawFramesCount + (actor->GetID().D & redrawFramesCount)))
        dirty = true;

    // Mark object as used
    object->Actor = actor;
    object->LastFrameUsed = surfaceAtlasData.CurrentFrame;
    object->Bounds = OrientedBoundingBox(localBounds);
    object->Bounds.Transform(localToWorld);
    object->Radius = (float)actorObjectBounds.Radius;
    if (dirty || GLOBAL_SURFACE_ATLAS_DEBUG_FORCE_REDRAW_TILES)
    {
        object->LastFrameDirty = surfaceAtlasData.CurrentFrame;
        _dirtyObjectsBuffer.Add(actorObject);
    }

    // Write to objects buffer (this must match unpacking logic in HLSL)
    Matrix localToWorldBounds;
    object->Bounds.Transformation.GetWorld(localToWorldBounds);
    Matrix worldToLocalBounds;
    Matrix::Invert(localToWorldBounds, worldToLocalBounds);
    uint32 objectAddress = _objectsBuffer->Data.Count() / sizeof(Float4);
    auto* objectData = _objectsBuffer->WriteReserve<Float4>(GLOBAL_SURFACE_ATLAS_OBJECT_DATA_STRIDE);
    objectData[0] = *(Float4*)&actorObjectBounds;
    objectData[1] = Float4::Zero; // w unused
    objectData[2] = Float4(worldToLocalBounds.M11, worldToLocalBounds.M12, worldToLocalBounds.M13, worldToLocalBounds.M41);
    objectData[3] = Float4(worldToLocalBounds.M21, worldToLocalBounds.M22, worldToLocalBounds.M23, worldToLocalBounds.M42);
    objectData[4] = Float4(worldToLocalBounds.M31, worldToLocalBounds.M32, worldToLocalBounds.M33, worldToLocalBounds.M43);
    objectData[5] = Float4(object->Bounds.Extents, 0.0f); // w unused
    auto tileOffsets = reinterpret_cast<uint16*>(&objectData[1]); // xyz used for tile offsets packed into uint16
    auto objectDataSize = reinterpret_cast<uint32*>(&objectData[1].W); // w used for object size (count of Float4s for object+tiles)
    *objectDataSize = GLOBAL_SURFACE_ATLAS_OBJECT_DATA_STRIDE;
    for (int32 tileIndex = 0; tileIndex < 6; tileIndex++)
    {
        auto* tile = object->Tiles[tileIndex];
        if (!tile)
            continue;
        tile->ObjectAddressOffset = *objectDataSize;
        tile->Address = objectAddress + tile->ObjectAddressOffset;
        tileOffsets[tileIndex] = tile->ObjectAddressOffset;
        *objectDataSize += GLOBAL_SURFACE_ATLAS_TILE_DATA_STRIDE;

        // Setup view to render object from the side
        Float3 xAxis, yAxis, zAxis = Float3::Zero;
        zAxis.Raw[tileIndex / 2] = tileIndex & 1 ? 1.0f : -1.0f;
        yAxis = tileIndex == 2 || tileIndex == 3 ? Float3::Right : Float3::Up;
        Float3::Cross(yAxis, zAxis, xAxis);
        Float3 localSpaceOffset = -zAxis * object->Bounds.Extents;
        xAxis = object->Bounds.Transformation.LocalToWorldVector(xAxis);
        yAxis = object->Bounds.Transformation.LocalToWorldVector(yAxis);
        zAxis = object->Bounds.Transformation.LocalToWorldVector(zAxis);
        xAxis.NormalizeFast();
        yAxis.NormalizeFast();
        zAxis.NormalizeFast();
        object->Bounds.Transformation.LocalToWorld(localSpaceOffset, tile->ViewPosition);
        tile->ViewDirection = zAxis;

        // Create view matrix
        tile->ViewMatrix.SetColumn1(Float4(xAxis, -Float3::Dot(xAxis, tile->ViewPosition)));
        tile->ViewMatrix.SetColumn2(Float4(yAxis, -Float3::Dot(yAxis, tile->ViewPosition)));
        tile->ViewMatrix.SetColumn3(Float4(zAxis, -Float3::Dot(zAxis, tile->ViewPosition)));
        tile->ViewMatrix.SetColumn4(Float4(0, 0, 0, 1));

        // Calculate object bounds size in the view
        OrientedBoundingBox viewBounds(object->Bounds);
        viewBounds.Transform(tile->ViewMatrix);
        Float3 viewExtent;
        viewExtent = viewBounds.Transformation.LocalToWorldVector(viewBounds.Extents);
        tile->ViewBoundsSize = viewExtent.GetAbsolute() * 2.0f;

        // Per-tile data
        const float tileWidth = (float)tile->Width - GLOBAL_SURFACE_ATLAS_TILE_PADDING;
        const float tileHeight = (float)tile->Height - GLOBAL_SURFACE_ATLAS_TILE_PADDING;
        auto* tileData = _objectsBuffer->WriteReserve<Float4>(GLOBAL_SURFACE_ATLAS_TILE_DATA_STRIDE);
        tileData[0] = Float4(tile->X, tile->Y, tileWidth, tileHeight) * surfaceAtlasData.ResolutionInv;
        tileData[1] = Float4(tile->ViewMatrix.M11, tile->ViewMatrix.M12, tile->ViewMatrix.M13, tile->ViewMatrix.M41);
        tileData[2] = Float4(tile->ViewMatrix.M21, tile->ViewMatrix.M22, tile->ViewMatrix.M23, tile->ViewMatrix.M42);
        tileData[3] = Float4(tile->ViewMatrix.M31, tile->ViewMatrix.M32, tile->ViewMatrix.M33, tile->ViewMatrix.M43);
        tileData[4] = Float4(tile->ViewBoundsSize, 0.0f); // w unused
    }
}
