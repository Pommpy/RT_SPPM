#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;

// used to show AS built for photons
cbuffer PerFrame
{
    float gGlobalRadius;
    float gCausticRadius;
    float3 gCamPos;
}

RWTexture2D<float4> gPhotonImage;
Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gViewWorld;

StructuredBuffer<AABB> gPhotonAABB[2];
RaytracingAccelerationStructure gPhotonAS;

struct RayData
{
    // we do not need to pass any value to the ray
    // What we need to do:
    // 1. Generate a ray from pinhole camera
    // 2. Get intersection
    // 3. Render closest hit, store color to the ray data
    float3 color;
};
struct SphereAttribs
{
    float2 pad;
};

[shader("closesthit")]
void photonASClosestHit(inout RayData rayData: SV_RayPayload, SphereAttribs attr: SV_IntersectionAttributes)
{
    rayData.color = InstanceIndex() == 0 ? float3(1.0f, 0.3f, 0.3f) : float3(0.3f, 1.0f, 0.3f);
}

[shader("closesthit")]
void triangleClosestHit(inout RayData rayData: SV_RayPayload, BuiltInTriangleIntersectionAttributes attribs)
{
    rayData.color = float3(attribs.barycentrics, 1 - attribs.barycentrics.x - attribs.barycentrics.y);
}

[shader("miss")]
void miss(inout RayData rayData: SV_RayPayload)
{
    rayData.color = float3(0.0f, 0.0f, 0.0f);
}

[shader("intersection")]
void intersection()
{
    SphereAttribs attribs;
    ReportHit(RayTCurrent(), 0, attribs);
    return;
    const float3 origin = ObjectRayOrigin();
    const uint primIndex = PrimitiveIndex();

    AABB photonAABB = gPhotonAABB[InstanceIndex()][PrimitiveIndex()];
    float radius = InstanceIndex() == 0 ? gCausticRadius: gGlobalRadius;
    float3 center = photonAABB.center();

    float3 dis = origin - center;
    dis = dis * dis;
    float distance = dis.x + dis.y + dis.z;
    if (distance < radius * radius)
    {
        SphereAttribs attribs;
        ReportHit(RayTCurrent(), 0, attribs);
    }
}

[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;

    const HitInfo hit = HitInfo(gVBuffer[pixel]);
    bool valid = hit.isValid();
    const TriangleHit triangleHit = hit.getTriangleHit();
    VertexData v = gScene.getVertexData(triangleHit);

    float3 color = (v.normalW + 1.0) / 2;
    color = (gViewWorld[pixel].xyz + 1.0) / 2;
    // gPhotonImage[pixel] = float4(color, 1.0);

    RayDesc rayDesc;
    rayDesc.Origin = gCamPos;
    rayDesc.Direction = -gViewWorld[pixel].xyz;
    rayDesc.TMin = 0;
    rayDesc.TMax = 10000000;
    uint rayFlags = RAY_FLAG_SKIP_TRIANGLES;
    // rayFlags = 0;
    RayData payload;
    //TraceRay(gScene.rtAccel, rayFlags, 0xff, 1, 0, 0, rayDesc, payload);

    // 
    TraceRay(gPhotonAS, rayFlags, 0xFF, 0, 0, 0, rayDesc, payload);

    // //gPhotonImage[pixel] = float4(1.0);
    gPhotonImage[pixel] = float4(payload.color, 1.0);
}
