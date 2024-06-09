#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import Scene.Material.ShadingUtils;
//import Experimental.Scene.Material.MaterialHelpers;
import Rendering.Lights.LightHelpers;
import Utils.Debug.PixelDebug;

cbuffer PerFrame
{
    uint gFrameCount; // Frame count since scene was updated
    float gCausticRadius;
    float gGlobalRadius;
    uint gSeed;
}

cbuffer CB
{
    //float gEmissiveScale; // Scale for the emissive part
    bool gCollectGlobalPhotons;
    bool gCollectCausticPhotons;
};

// inputs
Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gViewWorld;

// outputs
RWTexture2D<float4> gPhotonImage;

// Helper Structs
struct PhotonInfo
{
    float4 dir;
    float4 flux;
};
// Photon Buffers
Texture2D<float4> gPhotonFlux[2];
Texture2D<float4> gPhotonDir[2];
StructuredBuffer<AABB> gPhotonAABB[2];
RaytracingAccelerationStructure gPhotonAS;

#define is_valid(name) (is_valid_##name != 0)
static const uint kInfoTexHeight = INFO_TEX_HEIGHT;
static const float kRayTMax = FLT_MAX;

struct RayData // Ray payload
{
    float3 radiance;
    float pad0; // 16B
    PackedHitInfo packedHitInfo; // 16B
    SampleGenerator sg; // 16B

    // total: 36B
    __init(SampleGenerator sg)
    {
        this.radiance = 0;
        this.sg = sg;
    }
}

struct SphereAttribs
{
    float2 pad;
};
ShadingData loadShadingData(const HitInfo hit, const float3 rayOrigin, const float3 rayDir, const ITextureSampler lod)
{
    VertexData v = {};
    uint materialID = {};

#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_TRIANGLE_MESH)
    if (hit.getType() == HitType::Triangle)
    {
        const TriangleHit triangleHit = hit.getTriangleHit();
        v = gScene.getVertexData(triangleHit);
        materialID = gScene.getMaterialID(triangleHit.instanceID);
    }
#endif

#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_DISPLACED_TRIANGLE_MESH)
    if (hit.getType() == HitType::DisplacedTriangle)
    {
        const DisplacedTriangleHit displacedTriangleHit = hit.getDisplacedTriangleHit();
        v = gScene.getVertexData(displacedTriangleHit, -rayDir);
        materialID = gScene.getMaterialID(displacedTriangleHit.instanceID);
    }
#endif
    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -rayDir, lod);
    return sd;
}
[shader("anyhit")]
void anyHit(inout RayData rayData, SphereAttribs attribs)
{
    // if(InstanceIndex() == 0)
    //     rayData.radiance += float3(1.0, 0.3, 0.3);
    // else
    //     rayData.radiance += float3(0.0, 0.0, 0.0);
    // return;

    // We find a valid photon contribute to this ray
    // Accumulate photon contribution here
    const uint primIndex = PrimitiveIndex();
    const uint2 primIndex2D = uint2(primIndex / kInfoTexHeight, primIndex % kInfoTexHeight);
    float3 photonFlux = gPhotonFlux[InstanceIndex()][primIndex2D].xyz;
    float3 photonDir = gPhotonDir[InstanceIndex()][primIndex2D].xyz;

    const float3 primaryRayOrigin = WorldRayOrigin();
    const float3 primaryRayDir = WorldRayDirection();
    const HitInfo hit = HitInfo(rayData.packedHitInfo);
    let lod = ExplicitLodTextureSampler(0.f);
    ShadingData sd = loadShadingData(hit, primaryRayOrigin, primaryRayDir, lod); // This stage may be costly
    let mi = gScene.materials.getMaterialInstance(sd, lod);
    float3 bsdfValue = mi.eval(sd, -photonDir, rayData.sg); // bsdf * cos(n, photonDir)

    float3 photonContribution = photonFlux * bsdfValue;

    // rayData.radiance++;
    rayData.radiance += photonContribution;
}

[shader("intersection")]
void intersection()
{
    // SphereAttribs attribs;
    // ReportHit(RayTCurrent(), 0, attribs);
    // return;
    const float3 origin = WorldRayOrigin();
    const uint primIndex = PrimitiveIndex();

    AABB photonAABB = gPhotonAABB[InstanceIndex()][PrimitiveIndex()];
    float radius = InstanceIndex() == 0 ? gCausticRadius: gGlobalRadius;
    float3 center = photonAABB.center();

    float3 dis = origin - center;
    dis = dis * dis;
    float distance = dis.x + dis.y + dis.z;
    if(distance < radius * radius)
    {
        SphereAttribs attribs;
        ReportHit(RayTCurrent(), 0, attribs);
    }
}

[shader("miss")]
void miss(inout RayData rayData)
{

}

[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;
    SampleGenerator sg = SampleGenerator(pixel, gSeed);
    
    float3 outColor = float3(0.f);

    const HitInfo hit = HitInfo(gVBuffer[pixel]);
    bool valid = hit.isValid();
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);

    // rayData.radiance = gPhotonFlux[1][0].xyz * 100000000;
    // // rayData.radiance = 1.0;
    // gPhotonImage[pixel] = float4(rayData.radiance, 1.0);
    // return;
    // trace a ray from the primary hit to get nearby photon information
    RayDesc rayDesc;
    rayDesc.Origin = v.posW;
    rayDesc.Direction = -gViewWorld[pixel].xyz;
    rayDesc.TMin = 0;
    rayDesc.TMax = 1e9;
    uint rayFlags = RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_SKIP_TRIANGLES;

    float3 radiance = 0;
    if (gCollectCausticPhotons && valid)
    {
        RayData rayData = RayData(sg);
        rayData.packedHitInfo = gVBuffer[pixel];
        TraceRay(gPhotonAS, rayFlags, 1 /*instance mask for caustic photons*/, 0, 0, 0, rayDesc, rayData);
        float w = 1.0 / (M_PI * gCausticRadius * gCausticRadius);
        radiance += rayData.radiance * w;
        // radiance += rayData.radiance;
    }
    if (gCollectGlobalPhotons && valid)
    {
        RayData rayData = RayData(sg);
        rayData.packedHitInfo = gVBuffer[pixel];
        TraceRay(gPhotonAS, rayFlags, 2 /*instance mask for global photons*/, 0, 0, 0, rayDesc, rayData);
        float w = 1.0 / (M_PI * gGlobalRadius * gGlobalRadius);
        radiance += rayData.radiance * w;
        // radiance += rayData.radiance;
    }
    radiance = radiance / (512 * 512);
    // radiance = radiance / (radiance + 1);
    //float3 emission = gEmissive[pixel].xyz;
    //rayData.radiance += emission;

    if(gFrameCount > 0)
    {
        // Accumulate
        float3 last = gPhotonImage[pixel].xyz;
        float frameCountf = float(gFrameCount);
        last *= frameCountf;
        radiance += last;
        radiance /= frameCountf + 1.0;
    }
    gPhotonImage[pixel] = float4(radiance, 1.0);
}
