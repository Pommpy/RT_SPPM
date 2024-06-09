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
import Rendering.Lights.EmissiveUniformSampler; // Uniformly sample lights
import Rendering.RTXDI.RTXDI;

import Utils.Debug.PixelDebug;

cbuffer CB
{
    uint gFrameCount; // Frame count since scene was updated
    uint gPRNGDimension;
    uint gSeed;
}

// Inputs
Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gViewWorld;

RWTexture2D<float4> gOutputColor;

static EmissiveUniformSampler emissiveLightSampler;

// #define is_valid(name) (is_valid_##name != 0)
static const uint kMaxBounces = MAX_BOUNCES;
static const bool kComputeDirect = COMPUTE_DIRECT;
static const bool kUseImportanceSampling = USE_IMPORTANCE_SAMPLING;
static const bool kUseAnalyticLights = USE_ANALYTIC_LIGHTS;
static const bool kUseEmissiveLights = USE_EMISSIVE_LIGHTS;
static const bool kUseEnvLight = USE_ENV_LIGHT;
static const bool kUseEnvBackground = USE_ENV_BACKGROUND;
static const bool kUseMIS = USE_MIS;
static const bool kUseNEE = USE_NEE;
static const bool kUseRTXDI = USE_RTXDI;
static const float3 kDefaultBackgroundColor = float3(0, 0, 0);
static const float kRayTMax = FLT_MAX;

// Payload for shadow ray
struct ShadowRayData
{
    bool visible;
};

// Payload for scatter ray, max(128B)
struct ScatterRayData
{
    float3 radiance;
    bool terminated; // 4B
    float3 thp;

    float pathPdf; // 8B
    uint bounces;
    
    float3 origin; // 12B
    float3 direction;

    float3 lastVertexPos; // for evaluating MIS light PDF

    SampleGenerator sg; // Per-ray state for the sample generator

    // For debug
    uint2 pixel_xy;
    bool RTXDI_SAMPLE_SUCCESS = false;

    __init(SampleGenerator sg)
    {
        this.terminated = false;
        this.pathPdf = 1.0;
        this.bounces = 1;
        this.radiance = float3(0, 0, 0);
        this.thp = float3(1, 1, 1);
        this.origin = float3(0, 0, 0);
        this.direction = float3(0, 0, 0);
        this.sg = sg;

        this.RTXDI_SAMPLE_SUCCESS = false;

        this.lastVertexPos = float3(0, 0, 0);
    }
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

void setupPathLogging(const ScatterRayData rayData)
{
    printSetPixel(rayData.pixel_xy);
    //logSetPixel(rayData.pixel_xy);
}

float3 getPrimaryRayDir(uint2 launchIndex, uint2 launchDim, const Camera camera)
{
    // if (is_valid(gViewWorld))
    {
        return -gViewWorld[launchIndex].xyz;
    }
    // else
    // {
    //     return camera.computeRayPinhole(launchIndex, launchDim).dir;
    // }
}

bool traceShadowRay(float3 origin, float3 dir, float distance)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.f;
    ray.TMax = distance;

    ShadowRayData rayData;
    rayData.visible = false; // Set to true by miss shader if not intersected anything

    TraceRay(
        gScene.rtAccel,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
        0xff,
        1,
        rayTypeCount,
        1, // Miss Shader index
        ray,
        rayData
    );

    return rayData.visible;
}

void traceScatterRay(inout ScatterRayData rayData)
{
    RayDesc ray;
    ray.Origin = rayData.origin;
    ray.Direction = rayData.direction;
    ray.TMin = 0.f;
    ray.TMax = kRayTMax;

    uint rayFlags = 0;
    TraceRay(gScene.rtAccel, rayFlags, 0xff, 0 /*hit idx*/, rayTypeCount, 0 /* miss idx*/, ray, rayData);
}

bool generateScatterRay(const ShadingData sd, const IMaterialInstance mi, bool isCurveHit, float3 rayOrigin, inout ScatterRayData rayData)
{
    BSDFSample bsdfSample;
    if(mi.sample(sd, rayData.sg, bsdfSample, kUseImportanceSampling))
    {
        rayData.lastVertexPos = sd.posW;
        rayData.origin = rayOrigin;
        if(!isCurveHit && bsdfSample.isLobe(LobeType::Transmission))
        {
            rayData.origin = sd.computeRayOrigin(false);
        }
        rayData.direction = bsdfSample.wo;
        rayData.thp *= bsdfSample.weight;
        rayData.pathPdf = bsdfSample.pdf;

        if(any(rayData.thp > 0.f))
        {
            return true;
        }
    }
    return false;
}

float evalMIS(uint n0, float p0, uint n1, float p1){
    float q0 = n0 * p0;
    float q1 = n1 * p1;
    return q0 / (q0 + q1);
}

void handleHit(const HitInfo hit, inout ScatterRayData rayData) // at least second hit
{
    const bool isCurveHit = hit.getType() == HitType::Curve;
    let lod = ExplicitLodTextureSampler(0.f);

    // Load Shading data
    ShadingData sd = loadShadingData(hit, rayData.origin, rayData.direction, lod);

    let mi = gScene.materials.getMaterialInstance(sd, lod);

    // L_e
    BSDFProperties bsdfProperties = mi.getProperties(sd);
    if(kUseEmissiveLights && (rayData.bounces >= 2 || !kUseRTXDI) && any(bsdfProperties.emission > 0))
    {
        // For primary rays, we apply RTXDI
        // For secondary rays, we apply NEE
        // we hit the light
        float misWeight = 1;
        if(kUseNEE)
        {
            misWeight = 0;
            if(kUseMIS)
            {
                TriangleHit triangleHit = hit.getTriangleHit();
                TriangleLightHit triangleLightHit;
                triangleLightHit.triangleIndex = gScene.lightCollection.getTriangleIndex(triangleHit.instanceID, triangleHit.primitiveIndex);
                triangleLightHit.posW = sd.posW;
                triangleLightHit.normalW = sd.getOrientedFaceNormal();
                float lightPdf = emissiveLightSampler.evalPdf(rayData.lastVertexPos, 0.f, true, triangleLightHit);
                misWeight = evalMIS(1, rayData.pathPdf, 1, lightPdf);
                // misWeight = 1;
            }
        }
        rayData.radiance += rayData.thp * bsdfProperties.emission * misWeight;
    }

    // Check termination
    if(rayData.bounces >= kMaxBounces)
    {
        rayData.terminated = true;
        return;
    }

    // NEE
    if(kUseNEE)
    {
        TriangleLightSample ls = {};
        bool success = emissiveLightSampler.sampleLight(sd.posW, sd.faceN, true, rayData.sg, ls);
        ls.Le /= ls.pdf;
        if(success)
        {
            const float3 origin = computeRayOrigin(sd.posW, dot(sd.faceN, ls.dir) >= 0.f ? sd.faceN : -sd.faceN);
            float3 bsdfValue = mi.eval(sd, ls.dir, rayData.sg); // bsdf * cos(n, wo)

            if(kUseMIS)
            {
                float pathPdf = mi.evalPdf(sd, ls.dir, kUseImportanceSampling);
                ls.Le *= evalMIS(1, ls.pdf, 1, pathPdf);
            }
            
            bool V = traceShadowRay(origin, ls.dir, ls.distance * 0.99); // shadow ray
            if(V)
            {
                rayData.radiance += rayData.thp * ls.Le * bsdfValue;
            }
        }
    }

    float3 rayOrigin;
    if(isCurveHit)
    {
        // new origin at sphere center
        rayOrigin = sd.posW - sd.curveRadius * sd.frame.N;
    }
    else
    {
        rayOrigin = sd.computeRayOrigin();
    }


    if(!generateScatterRay(sd, mi, isCurveHit, rayOrigin, rayData))
    {
        rayData.terminated = true;
        return;
    }
    rayData.bounces++;
}

float3 tracePath(const uint2 pixel, const uint2 frameDim)
{
    float3 outColor = float3(0.f);

    const float3 primaryRayOrigin = gScene.camera.getPosition();
    const float3 primaryRayDir = getPrimaryRayDir(pixel, frameDim, gScene.camera);

    const HitInfo hit = HitInfo(gVBuffer[pixel]);
    if(hit.isValid())
    {
        const bool isCurveHit = hit.getType() == HitType::Curve;
        let lod = ExplicitLodTextureSampler(0.f);

        ShadingData sd = loadShadingData(hit, primaryRayOrigin, primaryRayDir, lod); // This stage may be costly

        let mi = gScene.materials.getMaterialInstance(sd, lod);

        SampleGenerator sg = SampleGenerator(pixel, gSeed);

        for(uint i = 0; i < gPRNGDimension; i++)
            sampleNext1D(sg);

        if (kUseRTXDI)
        {
            // RTXDI
            let bsdfProperties = mi.getProperties(sd);

            uint lobeTypes = mi.getLobeTypes(sd);
            bool validSurface =  (lobeTypes & (uint)LobeType::NonDeltaReflection) != 0;
            if (validSurface)
            {
                gRTXDI.setSurfaceData( pixel, sd.computeRayOrigin(), bsdfProperties.guideNormal, bsdfProperties.diffuseReflectionAlbedo, bsdfProperties.specularReflectance, bsdfProperties.roughness );   
            }
            else
            {
                gRTXDI.setInvalidSurfaceData(pixel);
            }
        }

        float3 rayOrigin;
        if (isCurveHit)
        {
            // For curves, we set the new origin at the sphere center.
            rayOrigin = sd.posW - sd.curveRadius * sd.frame.N;
        }
        else
        {
            rayOrigin = sd.computeRayOrigin();
        }

        ScatterRayData rayData = ScatterRayData(sg);
        // prepare ray payload
        rayData.pixel_xy = pixel;
        rayData.radiance += mi.getProperties(sd).emission;
        setupPathLogging(rayData);
        
        // RTXDI instead of NEE for primary rays
        if (kUseRTXDI)
        {
            TriangleLightSample ls = {};
            bool validSample = false;
            validSample = gRTXDI.getFinalSample(pixel, ls.dir, ls.distance, ls.Le);
            float3 bsdfValue = mi.eval(sd, ls.dir, rayData.sg);
            bool V = traceShadowRay(rayOrigin, ls.dir, ls.distance * 0.99); // shadow ray
            if (V && validSample)
            {
                rayData.radiance += rayData.thp * ls.Le * bsdfValue;
                rayData.RTXDI_SAMPLE_SUCCESS = true;
            }
            
        }
        else if(kUseNEE && rayData.bounces <= kMaxBounces)
        {
            // NEE here
            TriangleLightSample ls = {};
            bool success = emissiveLightSampler.sampleLight(sd.posW, sd.faceN, true, rayData.sg, ls);
            ls.Le /= ls.pdf;
            if(success)
            {
                const float3 origin = computeRayOrigin(sd.posW, dot(sd.faceN, ls.dir) >= 0.f ? sd.faceN : -sd.faceN);
                //float3 toLight = ls.dir;
                float3 bsdfValue = mi.eval(sd, ls.dir, rayData.sg); // bsdf * cos(n, wo)
                if(kUseMIS)
                {
                    float pathPdf = mi.evalPdf(sd, ls.dir, kUseImportanceSampling);
                    ls.Le *= evalMIS(1, ls.pdf, 1, pathPdf);
                }
                // const float3 origin = computeRayOrigin(sd.posW, dot(sd.faceN, ls.dir) >= 0.f ? sd.faceN : -sd.faceN);
                bool V = traceShadowRay(origin, ls.dir, ls.distance * 0.99); // shadow ray
                if(V)
                {
                    rayData.radiance += rayData.thp * ls.Le * bsdfValue;
                }
            }
        }

        if (!generateScatterRay(sd, mi, isCurveHit, rayOrigin, rayData))
        {
            rayData.terminated = true;
        }

        for(uint bounces = 1; bounces <= kMaxBounces && !rayData.terminated; bounces++)
        {
            traceScatterRay(rayData);
        }
        
        // // Debug print

            // found_pixel = rayData.pixel_xy;
        print("pixel", rayData.pixel_xy);
        print("RTXDI_SAMPLE_SUCCESS", rayData.RTXDI_SAMPLE_SUCCESS);
        // print("connect_length", rayData.connectLength);
        // print("hit_length", rayData.hitLength);
        // print("pixel_radiance", rayData.radiance);
        outColor += rayData.radiance;
    }
    else
    {
        outColor = kUseEnvBackground? gScene.envMap.eval(primaryRayDir): kDefaultBackgroundColor;
    }


    return outColor;
}

// Shader entry points
[shader("miss")]
void scatterMiss(inout ScatterRayData rayData)
{
    rayData.terminated = true;

    if(kUseEnvLight && (kComputeDirect || rayData.bounces > 0))
    {
        rayData.radiance += rayData.thp * gScene.envMap.eval(WorldRayDirection()); // why world ray direction here
    }
}

[shader("miss")]
void shadowMiss(inout ShadowRayData rayData)
{
    rayData.visible = true;
}

[shader("anyhit")]
void scatterTriangleMeshAnyHit(inout ScatterRayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // alpha test
    GeometryInstanceID instanceID = getGeometryInstanceID();
    VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(instanceID);
    if(gScene.materials.alphaTest(v, materialID, 0.f))
    {
        IgnoreHit();
    }
}

[shader("closesthit")]
void scatterTriangleMeshClosestHit(inout ScatterRayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;
    handleHit(HitInfo(triangleHit), rayData);
}

[shader("anyhit")]
void shadowTriangleMeshAnyHit(inout ShadowRayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // Alpha test for non-opaque geometry.
    GeometryInstanceID instanceID = getGeometryInstanceID();
    VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(instanceID);
    if (gScene.materials.alphaTest(v, materialID, 0.f))
        IgnoreHit();
}

[shader("raygeneration")]
void rayGen()
{
    // return;
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;


    // 1074 407

    // 1314 518
    float3 color = tracePath(pixel, frameDim);
    gOutputColor[pixel] = float4(color, 1.f);
}
