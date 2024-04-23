#include "SPPM.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "RenderGraph/RenderPassStandardFlags.h"

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry & registry)
{
    registry.registerClass<RenderPass, SPPM>();
    //ScriptBindings::registerBinding(MyPathTracer::registerBindings);
}

namespace
{
const char kTracePhoton[] = "RenderPasses/SPPM/TracePhoton.rt.slang";
const char kCollectPhoton[] = "RenderPasses/SPPM/CollectPhoton.rt.slang";
const uint32_t kMaxPayloadSize = 64u;
const uint32_t kMaxAttributeSize = 8u;
const uint32_t kMaxRecursionDepth = 5u;

const ChannelList kInputChannels =
{
    {"vbuffer",             "gVBuffer",                 "V Buffer to get the intersected triangle",         false},
    {"viewW",               "gViewWorld",               "World View Direction",                             false},
};

const ChannelList kOutputChannels =
{
    { "PhotonImage",          "gPhotonImage",               "An image that shows the caustics and indirect light from global photons" , false , ResourceFormat::RGBA32Float }
};
}

SPPM::SPPM(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice)
{
    mpSampleGenerator = SampleGenerator::create(mpDevice, SAMPLE_GENERATOR_UNIFORM);
    // don't need values in the script
    // control the parameters using ImGUI
}

RenderPassReflection SPPM::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;
    // Define input/output channels
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels);
    return reflector;
}

void SPPM::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    // Reset if options affecting output are changed
    auto& dict = renderData.getDictionary();
    if (mOptionChanged)
    {
        auto flags = dict.getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
        dict[Falcor::kRenderPassRefreshFlags] = flags | Falcor::RenderPassRefreshFlags::RenderOptionsChanged;
        mOptionChanged = false;

        mResetTimer = true;
        mResetIteration = true;
        mResetCB = true;
    }

    if (!mpScene)
    {
        return;
    }
    // Reset frame count if camera moves or option changes
    if (mResetIteration || is_set(mpScene->getUpdates(), Scene::UpdateFlags::CameraMoved))
    {
        mFrameCount = 0;
        mResetIteration = false;
        mResetTimer = true;
    }
    recordTimer();

    // TO DO: Output photon count to UI
    // TO DO: Control photon number in UI
    // TO DO: UI BLAS config
    // copyPhotonToUI();
    // updatePhotonNumber();
    // updateBLASConfig();

    if (mFrameCount == 0)
    {
        mCausticRadius = mCausticInitRadius;
        mGlobalRadius = mGlobalInitRadius;
    }
    // Request the light collection if emissive lights are enabled.
    if (mpScene->getRenderSettings().useEmissiveLights)
    {
        auto& pLights = mpScene->getLightCollection(pRenderContext);
        FALCOR_ASSERT(pLights && pLights->getActiveLightCount(pRenderContext) > 0);
        if (!mpEmissivePowerSampler)
        {
            mpEmissivePowerSampler = std::make_unique<EmissivePowerSampler>(pRenderContext, mpScene);
        }
    }
    if (mResizePhotonBuffer)
    {
        mResizePhotonBuffer = false;

        mCausticPhotonBuffers.size = mGlobalPhotonBuffers.size = mPhotonBufferWidth * mPhotonBufferHeight;
        mCreateBuffer = true;
        mRebuildAS = true;
    }
    if (mCreateBuffer)
    {
        preparePhotonBuffers(mCausticPhotonBuffers);
        preparePhotonBuffers(mGlobalPhotonBuffers);
        mCreateBuffer = false;
    }

    if (mRebuildAS)
    {
        prepareBLAS(mCausticPhotonBuffers);
        prepareBLAS(mGlobalPhotonBuffers);
        prepareTLAS(pRenderContext);
        mRebuildAS = false;
    }

    tracePhotonPass(pRenderContext, renderData);

    buildBLAS(pRenderContext, mCausticPhotonBuffers);
    buildBLAS(pRenderContext, mGlobalPhotonBuffers);
    buildTLAS(pRenderContext);

    collectPhotonPass(pRenderContext, renderData); // after building AS for photons, we can start camera tracing
    mFrameCount++;

    // update photon radius
    float itF = static_cast<float>(mFrameCount);
    mGlobalRadius *= sqrt((itF + mSPPMAlpha) / (itF + 1.0f));
    mCausticRadius *= sqrt((itF + mSPPMAlpha) / (itF + 1.0f));
    //Clamp to min radius
    mGlobalRadius = std::max(mGlobalRadius, kMinPhotonRadius);
    mCausticRadius = std::max(mCausticRadius, kMinPhotonRadius);
    if (mResetCB) mResetCB = false;
}

void SPPM::tracePhotonPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "tracePass");

    // Reset photon count buffer
    pRenderContext->copyBufferRegion(mPhotonCounter.get(), 0, mPhotonCounterReset.get(), 0, sizeof(uint64_t));
    pRenderContext->resourceBarrier(mPhotonCounter.get(), Resource::State::ShaderResource);

    // Clear photon buffers
    pRenderContext->clearUAV(mGlobalPhotonBuffers.aabbs.get()->getUAV().get(), uint4(0, 0, 0, 0));
    pRenderContext->clearTexture(mGlobalPhotonBuffers.flux.get());
    pRenderContext->clearTexture(mGlobalPhotonBuffers.dir.get());
    pRenderContext->clearUAV(mCausticPhotonBuffers.aabbs.get()->getUAV().get(), uint4(0, 0, 0, 0));
    pRenderContext->clearTexture(mCausticPhotonBuffers.flux.get());
    pRenderContext->clearTexture(mCausticPhotonBuffers.dir.get());

    mTracePhotonPass.pProgram->addDefine("USE_IMPORTANCE_SAMPLING", "1");
    mTracePhotonPass.pProgram->addDefine("INFO_TEX_HEIGHT", std::to_string(mPhotonBufferHeight));
    mTracePhotonPass.pProgram->addDefine("TOTAL_PHOTON_COUNT", std::to_string(256 * 256));

    if (!mTracePhotonPass.pVars)
        prepareVars(mTracePhotonPass);
    FALCOR_ASSERT(mTracePhotonPass.pVars);

    auto var = mTracePhotonPass.pVars->getRootVar();
    var["PerFrame"]["gFrameCount"] = mFrameCount;
    var["PerFrame"]["gGlobalRadius"] = mGlobalRadius;
    var["PerFrame"]["gCausticRadius"] = mCausticInitRadius;
    if (mResetCB)
    {
        var["CB"]["gSeed"] = mUseFixedSeed ? 0 : mFrameCount;
        var["CB"]["gUseAlphaTest"] = mUseAlphaTest;
        var["CB"]["gSpecRoughCutoff"] = 0.5f;
        var["CB"]["gDepth"] = mDepth;
        FALCOR_ASSERT(mpEmissivePowerSampler);
        //mpEmissivePowerSampler->bindShaderData(var["CB"]["gEmissiveSampler"]);
    }

    for (int i = 0; i < 2; i++)
    {
        auto& buffer = (i == 0) ? mCausticPhotonBuffers : mGlobalPhotonBuffers;
        var["gPhotonAABB"][i] = buffer.aabbs;
        var["gPhotonFlux"][i] = buffer.flux;
        var["gPhotonDir"][i] = buffer.dir;
    }

    var["gPhotonCounter"] = mPhotonCounter;

    const uint2 targetDim = uint2(256, 256); // trace 2^18 photons, may store 2^18 * (depth = 4) = 2^20 photons

    mpScene->raytrace(pRenderContext, mTracePhotonPass.pProgram.get(), mTracePhotonPass.pVars, uint3(targetDim, 1));
}

void SPPM::collectPhotonPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "CollectPass");

    mCollectPhotonPass.pProgram->addDefines(getValidResourceDefines(kInputChannels, renderData));
    mCollectPhotonPass.pProgram->addDefines(getValidResourceDefines(kOutputChannels, renderData));
    mCollectPhotonPass.pProgram->addDefine("INFO_TEX_HEIGHT", std::to_string(mPhotonBufferHeight));

    if (!mCollectPhotonPass.pVars)
        prepareVars(mCollectPhotonPass);
    FALCOR_ASSERT(mCollectPhotonPass.pVars);

    auto var = mCollectPhotonPass.pVars->getRootVar();

    var["PerFrame"]["gFrameCount"] = mFrameCount;
    var["PerFrame"]["gCausticRadius"] = mCausticRadius;
    var["PerFrame"]["gGlobalRadius"] = mGlobalRadius;
    if (mResetCB)
    {
        var["CB"]["gSeed"] = mUseFixedSeed ? 0 : mFrameCount;
        var["CB"]["gCollectGlobalPhotons"] = true;
        var["CB"]["gCollectCausticPhotons"] = true;
    }
    // Bind I/O buffers
    auto bind = [&](const ChannelDesc& desc)
    {
        if (!desc.texname.empty())
        {
            var[desc.texname] = renderData.getTexture(desc.name);
        }
    };
    for (auto channel : kInputChannels)
        bind(channel);
    for (auto channel : kOutputChannels)
        bind(channel);

    var["gPhotonAS"].setAccelerationStructure(mTlasInfo.falcorTlas);

    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

    FALCOR_ASSERT(pRenderContext && mCollectPhotonPass.pProgram && mCollectPhotonPass.pVars);
    mpScene->raytrace(pRenderContext, mCollectPhotonPass.pProgram.get(), mCollectPhotonPass.pVars, uint3(targetDim, 1));
}

void SPPM::buildBLAS(RenderContext* pRenderContext, PhotonBuffers& photonBuffers)
{
    //pRenderContext->resourceBarrier(photonBuffers.aabbs.get(), Resource::State::NonPixelShader);
    FALCOR_PROFILE(pRenderContext, "buildPhotonBlas");
    auto& blasInfo = photonBuffers.blasInfo;
    pRenderContext->uavBarrier(photonBuffers.blasScratch.get());
    uint maxPhotons = mPhotonBufferWidth * mPhotonBufferHeight; // may be too big
    blasInfo.geoDescs.content.proceduralAABBs.count = maxPhotons;
    RtAccelerationStructure::BuildDesc asBuildDesc = {};
    asBuildDesc.inputs = blasInfo.inputs;
    asBuildDesc.scratchData = photonBuffers.blasScratch->getGpuAddress();
    asBuildDesc.dest = photonBuffers.falcorBlas.get();

    pRenderContext->buildAccelerationStructure(asBuildDesc, 0, nullptr);
    pRenderContext->uavBarrier(photonBuffers.blasBuffer.get());
}
void SPPM::buildTLAS(RenderContext* pRenderContext)
{
    FALCOR_PROFILE(pRenderContext, "buildPhotonTlas");
    pRenderContext->uavBarrier(mTlasInfo.pScratch.get());
    RtAccelerationStructure::BuildDesc asBuildDesc = {};
    asBuildDesc.inputs = mTlasInfo.inputs;
    asBuildDesc.dest = mTlasInfo.falcorTlas.get();
    asBuildDesc.scratchData = mTlasInfo.pScratch->getGpuAddress();
    asBuildDesc.inputs.instanceDescs = mTlasInfo.pInstanceDescs->getGpuAddress();

    pRenderContext->buildAccelerationStructure(asBuildDesc, 0, nullptr);
    pRenderContext->uavBarrier(mTlasInfo.pTlasBuffer.get());
}

void SPPM::prepareVars(SubPass& pass)
{
    FALCOR_ASSERT(mpScene);
    FALCOR_ASSERT(pass.pProgram);

    pass.pProgram->addDefines(mpSampleGenerator->getDefines());

    pass.pProgram->setTypeConformances(mpScene->getTypeConformances());

    pass.pVars = RtProgramVars::create(mpDevice, pass.pProgram, pass.pBindingTable);

    auto var = pass.pVars->getRootVar();
    mpSampleGenerator->bindShaderData(var);
}
void SPPM::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    resetSPPM();

    mpScene = pScene;
    mpScene->setIsAnimated(false);

    mTracePhotonPass.init();
    mCollectPhotonPass.init();

    if (mpScene)
    {
        auto typeConformance = pScene->getMaterialSystem().getTypeConformances();
        // create programs
        {
            // photon trace program
            ProgramDesc desc;
            desc.addShaderModules(mpScene->getShaderModules());
            desc.addShaderLibrary(kTracePhoton);
            desc.setMaxAttributeSize(kMaxAttributeSize);
            desc.setMaxPayloadSize(kMaxPayloadSize);
            desc.setMaxTraceRecursionDepth(mDepth);
            mTracePhotonPass.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
            auto& sbt = mTracePhotonPass.pBindingTable;
            sbt->setRayGen(desc.addRayGen("rayGen"));
            sbt->setMiss(0, desc.addMiss("miss"));
            if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
            {
                sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("closestHit", "anyHit"));
            }
            mTracePhotonPass.pProgram = Program::create(mpDevice, desc, mpScene->getSceneDefines());
        }
        {
            // photon collect program
            ProgramDesc desc;
            desc.addShaderModules(mpScene->getShaderModules());
            desc.addShaderLibrary(kCollectPhoton);
            desc.setMaxAttributeSize(kMaxAttributeSize);
            desc.setMaxPayloadSize(48); // 48B for collecting photon
            desc.setMaxTraceRecursionDepth(2); // Only for primary hit
            mCollectPhotonPass.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
            auto& sbt = mCollectPhotonPass.pBindingTable;
            sbt->setRayGen(desc.addRayGen("rayGen"));
            sbt->setMiss(0, desc.addMiss("miss"));

            // No closest shader (because we are using photons for primary rays, No final gather now)
            // We only need intersection shader to do ray-sphere intersection and anyhit shader to accumulate flux
            sbt->setHitGroup(0, 0 /*why is it 0 here*/, desc.addHitGroup("", "anyHit", "intersection"));
            mCollectPhotonPass.pProgram = Program::create(mpDevice, desc, mpScene->getSceneDefines());
        }
    }
    mPhotonCounter = mpDevice->createStructuredBuffer(sizeof(uint), 2);
    uint64_t zero = 0;
    mPhotonCounterReset = mpDevice->createBuffer(sizeof(uint64_t), ResourceBindFlags::None, MemoryType::DeviceLocal, &zero);
}
void SPPM::renderUI(Gui::Widgets& widget)
{

}

void SPPM::resetSPPM()
{
    mFrameCount = 0;

    mOptionChanged = true;
    mResetCB = true;
    mRebuildAS = true;
    mResetIteration = true;
    mCreateBuffer = true;
    mResetCB = true;
    mResetTimer = true;
}

void SPPM::preparePhotonTextures(PhotonBuffers& photonBuffers)
{
    // create buffers for photon flux and direction
    FALCOR_ASSERT(photonBuffers.size > 0);
    photonBuffers.flux.reset();
    photonBuffers.dir.reset();
    FALCOR_ASSERT(photonBuffers.size == mPhotonBufferWidth * mPhotonBufferHeight);
    photonBuffers.flux = mpDevice->createTexture2D(mPhotonBufferWidth, mPhotonBufferHeight, ResourceFormat::RGBA32Float, 1, 1, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    FALCOR_ASSERT(photonBuffers.flux);
    photonBuffers.dir = mpDevice->createTexture2D(mPhotonBufferWidth, mPhotonBufferHeight, ResourceFormat::RGBA32Float, 1, 1, nullptr, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    FALCOR_ASSERT(photonBuffers.dir);
}
void SPPM::preparePhotonBuffers(PhotonBuffers& photonBuffers)
{
    uint size = photonBuffers.size;
    photonBuffers.aabbs.reset();
    photonBuffers.blasBuffer.reset();
    
    photonBuffers.aabbs = mpDevice->createStructuredBuffer(sizeof(RtAABB), size);
    photonBuffers.aabbs->setName("photon aabbs");
    FALCOR_ASSERT(photonBuffers.aabbs);
    preparePhotonTextures(photonBuffers);
}

void SPPM::prepareTLAS(RenderContext* pRenderContext)
{
    {
        RtInstanceDesc causticDesc = {};
        causticDesc.accelerationStructure = mGlobalPhotonBuffers.blasBuffer->getGpuAddress();
        causticDesc.flags = RtGeometryInstanceFlags::None;
        causticDesc.instanceID = 0;
        causticDesc.instanceMask = 1;
        causticDesc.instanceContributionToHitGroupIndex = 0;
        float4x4 tranform;
        std::memcpy(causticDesc.transform, &tranform, sizeof(causticDesc.transform));
        photonInstanceDescs.push_back(causticDesc);
    }
    {
        RtInstanceDesc globalDesc = {};
        globalDesc.accelerationStructure = mGlobalPhotonBuffers.blasBuffer->getGpuAddress();
        globalDesc.flags = RtGeometryInstanceFlags::None;
        globalDesc.instanceID = 1;
        globalDesc.instanceMask = 2;
        globalDesc.instanceContributionToHitGroupIndex = 0;
        float4x4 tranform;
        std::memcpy(globalDesc.transform, &tranform, sizeof(globalDesc.transform));
        photonInstanceDescs.push_back(globalDesc);
    }
    RtAccelerationStructureBuildInputs inputs = {};
    inputs.kind = RtAccelerationStructureKind::TopLevel;
    inputs.descCount = (uint32_t)photonInstanceDescs.size();
    inputs.flags = RtAccelerationStructureBuildFlags::PreferFastBuild;
    mTlasInfo.pInstanceDescs = mpDevice->createBuffer((uint32_t)photonInstanceDescs.size() * sizeof(RtInstanceDesc), ResourceBindFlags::None, MemoryType::Upload, photonInstanceDescs.data());
    pRenderContext->resourceBarrier(mTlasInfo.pInstanceDescs.get(), Resource::State::NonPixelShader);
    inputs.instanceDescs = mTlasInfo.pInstanceDescs->getGpuAddress();
    inputs.geometryDescs = nullptr;

    mTlasInfo.inputs = inputs;
    mTlasInfo.prebuildInfo = RtAccelerationStructure::getPrebuildInfo(mpDevice.get(), inputs);
    uint scratchSize = std::max(mTlasInfo.prebuildInfo.scratchDataSize, mTlasInfo.prebuildInfo.updateScratchDataSize);
    uint tlasSize = mTlasInfo.prebuildInfo.resultDataMaxSize;
    mTlasInfo.pScratch = mpDevice->createBuffer(scratchSize);
    mTlasInfo.pTlasBuffer = mpDevice->createBuffer(tlasSize, ResourceBindFlags::AccelerationStructure);
    
    RtAccelerationStructure::Desc asDesc = {};
    asDesc.setBuffer(mTlasInfo.pTlasBuffer, 0, tlasSize);
    asDesc.setKind(RtAccelerationStructureKind::TopLevel);
    mTlasInfo.falcorTlas = RtAccelerationStructure::create(mpDevice, asDesc);
}
void SPPM::prepareBLAS(PhotonBuffers& photonBuffers)
{
    auto& blasInfo = photonBuffers.blasInfo;
    auto& desc = blasInfo.geoDescs;
    desc.type = RtGeometryType::ProcedurePrimitives;
    desc.flags = RtGeometryFlags::NoDuplicateAnyHitInvocation; // Each photon appears exactly once in anyhit shader
    desc.content.proceduralAABBs.count = photonBuffers.size;
    desc.content.proceduralAABBs.data = photonBuffers.aabbs->getGpuAddress();
    desc.content.proceduralAABBs.stride = sizeof(RtAABB);

    auto& inputs = blasInfo.inputs;
    inputs.kind = RtAccelerationStructureKind::BottomLevel;
    inputs.descCount = 1;
    inputs.geometryDescs = &desc;
    inputs.flags = RtAccelerationStructureBuildFlags::PreferFastBuild; // Because we always need to enumerate all leaves, we choose fast build (maybe LBVH?)

    blasInfo.prebuildInfo = RtAccelerationStructure::getPrebuildInfo(mpDevice.get(), inputs);
    FALCOR_ASSERT(blasInfo.prebuildInfo.resultDataMaxSize > 0);

    blasInfo.scratchBufferSize = std::max(blasInfo.prebuildInfo.scratchDataSize, blasInfo.prebuildInfo.updateScratchDataSize);
    blasInfo.scratchBufferSize = align_to(kAccelerationStructureByteAlignment, blasInfo.scratchBufferSize);
    blasInfo.blasSize = blasInfo.prebuildInfo.resultDataMaxSize;
    blasInfo.blasSize = align_to(kAccelerationStructureByteAlignment, blasInfo.blasSize);
    // Create scratch buffer and result buffer
    photonBuffers.blasScratch = mpDevice->createBuffer(blasInfo.scratchBufferSize);
    photonBuffers.blasBuffer = mpDevice->createBuffer(blasInfo.prebuildInfo.resultDataMaxSize, ResourceBindFlags::AccelerationStructure);
    RtAccelerationStructure::Desc asDesc = {};
    asDesc.setBuffer(photonBuffers.blasBuffer, 0, blasInfo.prebuildInfo.resultDataMaxSize);
    asDesc.setKind(RtAccelerationStructureKind::BottomLevel);
    photonBuffers.falcorBlas = RtAccelerationStructure::create(mpDevice, asDesc);
}

void SPPM::recordTimer()
{
    if (!mUseTimer) return;
    if (mResetTimer)
    {
        mTimer.startTime = std::chrono::steady_clock::now();
        mTimer.timesList = {};
        mResetTimer = false;
    }
    auto now = std::chrono::steady_clock::now();
    auto sec = (now - mTimer.startTime).count();
    mTimer.timesList.push_back(sec);
}
