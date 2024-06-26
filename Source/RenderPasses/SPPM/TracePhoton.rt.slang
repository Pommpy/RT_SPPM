#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Geometry.GeometryHelpers;
import Utils.Sampling.SampleGenerator;
import Rendering.Lights.LightHelpers;
import Rendering.Lights.EmissiveLightSampler;
import Rendering.Lights.EmissiveLightSamplerHelpers;
import Rendering.Lights.EmissivePowerSampler;


cbuffer PerFrame
{
    uint gFrameCount; // Frame count since scene was updated
    float gCausticRadius;
    float gGlobalRadius;
    uint gSeed;
}

// For light sampling
EmissivePowerSampler gEmissiveSampler;
cbuffer CB
{
    uint gDepth; // max photon depth
    bool gUseAlphaTest;
    float gSpecRoughCutoff;    // used to decide whether the material is rough enough
}
struct PhotonInfo
{
    float3 flux;
    float3 dir;
};


struct MyLightSample
{
    float3 pos;
    float3 dir;
    float3 thp;
};

RWStructuredBuffer<PhotonInfo> gPhotonInfo[2]; // 0-caustic, 1-global

// RWTexture2D<float4> gPhotonFlux[2]; // 0-caustic, 1-global
// RWTexture2D<float4> gPhotonDir[2];
RWStructuredBuffer<AABB> gPhotonAABB[2];
RWStructuredBuffer<uint> gPhotonCounter; // For all types of photons

RWTexture2D<float4> gPhotonImage;

Texture2D<uint> gSeeds; // we need a random seed buffer for light sampling

#define is_valid(name) (is_valid_##name != 0)
static const float kRayTMax = FLT_MAX;
static const bool kUseImportanceSampling = USE_IMPORTANCE_SAMPLING;
static const uint kTotalPhoton = TOTAL_PHOTON_COUNT;
struct RayData
{
    float3 thp;
    bool terminated; // 16B
    float3 origin;
    float pad0; // 16B
    float3 direction;
    bool diffuseHit; // 16B, record whether the hitted surface is diffuse

    bool allSpecular;
    SampleGenerator sg; // Per ray sampler, (up to 16B)
    
    // total 64B

    __init(SampleGenerator sg)
    {
        this.terminated = false;
        this.thp = float3(0, 0, 0);
        this.origin = float3(0, 0, 0);
        this.direction = float3(0, 0, 0);
        this.sg = sg;
        this.allSpecular = false;
    }
}

float3 toGlobal(float3 localDir, float3 N)
{
    float3 B = perp_stark(N);
    float3 T = cross(B, N);
    return B * localDir.x + T * localDir.y + N * localDir.z;
}
bool SampleLight(out MyLightSample ls, inout SampleGenerator sg)
{
    // We are using power sampler (i.e., sampling lights based on their flux)
    if (gScene.lightCollection.isEmpty()) return false;
    // Randomly pick a triangle out of the global list with uniform probability.
    float uLight = sampleNext1D(sg);
    uint triangleCount = gScene.lightCollection.triangleCount;
    // Safety precaution as the result of the multiplication may be rounded to triangleCount even if uLight < 1.0 when triangleCount is large.
    uint triangleIndex = min((uint)(uLight * triangleCount), triangleCount - 1);

    uint2 packed = gEmissiveSampler._emissivePower.triangleAliasTable[triangleIndex];
    float threshold = f16tof32(packed.x >> 16u);
    uint selectAbove = ((packed.x & 0xFFFFu) << 8u) | ((packed.y >> 24u) & 0xFFu);
    uint selectBelow = packed.y & 0xFFFFFFu;
    // Test the threshold in the current table entry; pick one of the two options
    triangleIndex = (sampleNext1D(sg) >= threshold) ? selectAbove : selectBelow;
    float triangleSelectionPdf = gScene.lightCollection.fluxData[triangleIndex].flux * gEmissiveSampler._emissivePower.invWeightsSum;

    // float triangleSelectionPdf = 1.0 / triangleCount;
    const EmissiveTriangle tri = gScene.lightCollection.getTriangle(triangleIndex);
    // after getting a triangle, we need to sample the position
    float2 u = sampleNext2D(sg);
    // Sample the triangle uniformly.
    const float3 barycentrics = sample_triangle(u);
    float3 pos = tri.getPosition(barycentrics);
    float3 normal = normalize(tri.normal);

    // after getting the position, we need to sample the direction
    float2 v = sampleNext2D(sg);
    float3 localDir;
    float dirPdf;
    localDir = sample_cosine_hemisphere_concentric(v, dirPdf);
    float3 dir = toGlobal(localDir, normal);

    // float samplePdf = triangleSelectionPdf * posPdf * dirPdf;
    // float samplePdf = posPdf * dirPdf;
    float samplePdf = triangleSelectionPdf * dirPdf;
    // need to calculate ray origin to avoid self intersection
    ls.pos = pos + dir * 0.01;
    // ls.pos = pos;
    ls.dir = dir;
    float2 uv = tri.getTexCoord(barycentrics);
    ls.thp = gScene.materials.evalEmissive(tri.materialID, uv) * max(localDir.z, 0.0f) * tri.area;
    // ls.thp = gScene.materials.evalEmissive(tri.materialID, uv);
    ls.thp /= samplePdf;

    return true;
}

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
bool generateScatterRay(const ShadingData sd, const IMaterialInstance mi, float3 rayOrigin, inout RayData rayData)
{
    BSDFSample bsdfSample;
    BSDFProperties bsdfProperties = mi.getProperties(sd);
    if (mi.sample(sd, rayData.sg, bsdfSample, kUseImportanceSampling))
    {
        // If this hit is glossy
        rayData.origin = rayOrigin;
        if (bsdfSample.isLobe(LobeType::Transmission))
        {
            rayData.origin = sd.computeRayOrigin(false);
        }
        rayData.diffuseHit = bsdfSample.isLobe(LobeType::Diffuse) || (bsdfSample.isLobe(LobeType::Reflection) && (bsdfProperties.roughness > gSpecRoughCutoff));
        rayData.direction = bsdfSample.wo;
        rayData.thp *= bsdfSample.weight;
        if (any(rayData.thp > 0.f))
        {
            return true;
        }
    }
    return false;
}

void handleHit(const HitInfo hit, inout RayData rayData)
{

    // generate scatter ray and update ray data
    let lod = ExplicitLodTextureSampler(0.f);

    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);
    // We get the photon information for this hit, now store the photon
    // invPhotonCount = 1;
    uint photonIndex = 0;
    uint insertIndex = rayData.allSpecular ? 0 : 1;
    float radius = insertIndex == 0 ? gCausticRadius : gGlobalRadius; // 0 caustic, 1 global

    AABB photonAABB = AABB(v.posW - radius, v.posW + radius);
    InterlockedAdd(gPhotonCounter[insertIndex], 1u, photonIndex);
   // uint2 index2D = uint2(photonIndex / kInfoTexHeight, photonIndex % kInfoTexHeight);

    gPhotonInfo[insertIndex][photonIndex] = {rayData.thp, rayData.direction};
    gPhotonAABB[insertIndex][photonIndex] = photonAABB;

    // Load Shading data
    ShadingData sd = loadShadingData(hit, rayData.origin, rayData.direction, lod);
    let mi = gScene.materials.getMaterialInstance(sd, lod);
    float3 rayOrigin;
    rayOrigin = sd.computeRayOrigin();
    if (!generateScatterRay(sd, mi, rayOrigin, rayData))
    {
        rayData.terminated = true;
        return;
    }
}

[shader("miss")]
void miss(inout RayData rayData: SV_RayPayload)
{
    rayData.terminated = true;
}
// Use anyhit for alpha test if activated
[shader("anyhit")]
void anyHit(inout RayData rayData: SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs: SV_IntersectionAttributes)
{
    if (gUseAlphaTest)
    {
        // Alpha test for non-opaque geometry.
        GeometryInstanceID instanceID = getGeometryInstanceID();
        VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
        uint materialID = gScene.getMaterialID(instanceID);
        if (gScene.materials.alphaTest(v, materialID, 0.f))
            IgnoreHit();
    }
}
[shader("closesthit")]
void closestHit(inout RayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;
    handleHit(HitInfo(triangleHit), rayData);
}


void tracePhoton(inout RayData rayData)
{
    for (int i = 0; i < gDepth && !rayData.terminated; i++)
    {
        RayDesc rayDesc;
        rayDesc.Origin = rayData.origin;
        rayDesc.Direction = rayData.direction;
        rayDesc.TMin = 0;
        rayDesc.TMax = kRayTMax;
        uint rayFlags = 0;
        TraceRay(gScene.rtAccel, rayFlags, 0xff, 0, rayTypeCount, 0, rayDesc, rayData);
        if (i == 0)
        {
            rayData.allSpecular = !rayData.diffuseHit;
        }   
        else
        {
            rayData.allSpecular &= (!rayData.diffuseHit);
        }
        
    }
}
[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;

    uint seed = gSeeds[pixel];
    SampleGenerator sg = SampleGenerator(pixel, seed + gSeed);

    RayData rayData = RayData(sg);
    MyLightSample ls;
    bool success = SampleLight(ls, sg);

    // if(success)
    // {
    //     gPhotonImage[pixel] = float4(ls.thp, 1.0);
    //     //gPhotonImage[pixel] = float4(1.0, 0.0, 0.0, 0.0);
    // }
    // else
    // {
    //     gPhotonImage[pixel] = float4(0.0, 1.0, 0.0, 0.0);
    // }
    // return;
    // Init ray data
    if(success)
    {
        rayData.origin = ls.pos;
        rayData.thp = ls.thp / (frameDim.x * frameDim.y);
        rayData.direction = ls.dir;
        tracePhoton(rayData);
    }
    // float3 color = (rayData.normal + 1) / 2;
    // // gPhotonImage[pixel] = float4(rayData.thp, 1.0);
    // float3 color = (float)gPhotonCounter[0] / ((float)gPhotonCounter[0] + 1.0f); 
    // gPhotonImage[pixel] = float4(color, 1.0);
    // if (gPhotonCounter[0] > 0)
    //     gPhotonImage[pixel] = float4(1.0);
    // check whether light sampling succeeded
    // if(success)
    // {
    //     uint photonIndex = 0;
    //     InterlockedAdd(gPhotonCounter[0], 1u, photonIndex);
    //     uint2 index2D = uint2(photonIndex / kInfoTexHeight, photonIndex % kInfoTexHeight);
    //     gPhotonFlux[0][index2D] = float4(ls.thp, 1.0);
    //     //gPhotonFlux[0][index2D] = 1;
    // }
    
}

