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
const char kShowAS[] = "RenderPasses/SPPM/ShowAS.rt.slang";
const uint32_t kMaxPayloadSize = 128u;
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
    mpPixelStats = std::make_unique<PixelStats>(mpDevice);
    mpPixelDebug = std::make_unique<PixelDebug>(mpDevice);
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

void SPPM::resetPhotonCounter(RenderContext* pRenderContext)
{
    // prepare photon counter
    {
        mPhotonCounter.counter = mpDevice->createStructuredBuffer(sizeof(uint), 2);
        uint64_t zero = 0;
        mPhotonCounter.reset = mpDevice->createBuffer(sizeof(uint64_t), ResourceBindFlags::None, MemoryType::DeviceLocal, &zero);
        //uint32_t oneInit[2] = { 1,1 };
        mPhotonCounter.cpuReadback = mpDevice->createBuffer(sizeof(uint64_t), ResourceBindFlags::None, MemoryType::ReadBack, nullptr);
    }
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
    // TO DO: UI BLAS config
    // copyPhotonToUI();
    // updatePhotonNumber();
    // updateBLASConfig();

    if (mFrameCount == 0)
    {
        mCausticRadius = mCausticInitRadius;
        mGlobalRadius = mGlobalInitRadius;
        resetPhotonCounter(pRenderContext);
        mPhotonCounts[0] = mPhotonCounts[1] = photonNumX * photonNumX * 4; // used for building AS for the first frame
    }
    // Request the light collection if emissive lights are enabled.
    if (mpScene->getRenderSettings().useEmissiveLights)
    {
        auto& pLights = mpScene->getLightCollection(pRenderContext);
        FALCOR_ASSERT(pLights && pLights->getActiveLightCount(pRenderContext) > 0);
        if (!mpEmissivePowerSampler)
        {
            mpEmissivePowerSampler = std::make_unique<EmissivePowerSampler>(pRenderContext, mpScene);
            mpEmissivePowerSampler->update(pRenderContext);
        }
    }
    //if (numPhotonChanged)
    //{
    //    // we may need to resize the photon buffer and AS buffer later
    //    mRebuildAS = true;
    //}

    if (updateMaxPhotonCount)
    {
        // If the maximum photon count is updated
        // It means that we want to resize the photon buffer and blas
        updateMaxPhotonCount = false;
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
        // estimate photons based on last iteration
        prepareBLAS(mCausticPhotonBuffers);
        prepareBLAS(mGlobalPhotonBuffers);
        prepareTLAS(pRenderContext);
        mRebuildAS = false;
    }

    //mpEmissivePowerSampler->update(pRenderContext);
    tracePhotonPass(pRenderContext, renderData);

    // estimate photons based on last iteration
    mPhotonASSizes.clear();
    mCausticPhotonBuffers.maxPhotonCount = std::min((uint)(mPhotonCounts[0] * photonASScale), mMaxPhotonCount * mMaxPhotonCount);
    mGlobalPhotonBuffers.maxPhotonCount = std::min((uint)(mPhotonCounts[1] * photonASScale), mMaxPhotonCount * mMaxPhotonCount);
    buildBLAS(pRenderContext, mCausticPhotonBuffers);
    buildBLAS(pRenderContext, mGlobalPhotonBuffers);
    buildTLAS(pRenderContext);

    //showASPass(pRenderContext, renderData);
    collectPhotonPass(pRenderContext, renderData); // after building AS for photons, we can start camera tracing
    mFrameCount++;

    // copy photon counter to CPU read back buffer and shown in UI later
    pRenderContext->copyBufferRegion(mPhotonCounter.cpuReadback.get(), 0, mPhotonCounter.counter.get(), 0, sizeof(uint64_t));
    void* data = mPhotonCounter.cpuReadback->map();
    std::memcpy(mPhotonCounts.data(), data, sizeof(uint32_t) * 2);

    // update photon radius
    float itF = static_cast<float>(mFrameCount);
    mGlobalRadius *= sqrt((itF + mSPPMAlpha) / (itF + 1.0f));
    mCausticRadius *= sqrt((itF + mSPPMAlpha) / (itF + 1.0f));
    //Clamp to min radius
    mGlobalRadius = std::max(mGlobalRadius, kMinPhotonRadius);
    mCausticRadius = std::max(mCausticRadius, kMinPhotonRadius);
    if (mResetCB)
        mResetCB = false;
}

void SPPM::tracePhotonPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "tracePass");
    auto defines = mpEmissivePowerSampler->getDefines();
    mTracePhotonPass.pProgram->addDefines(defines);

    // Reset photon count buffer
    pRenderContext->copyBufferRegion(mPhotonCounter.counter.get(), 0, mPhotonCounter.reset.get(), 0, sizeof(uint64_t));
    pRenderContext->resourceBarrier(mPhotonCounter.counter.get(), Resource::State::ShaderResource);

    mTracePhotonPass.pProgram->addDefine("USE_IMPORTANCE_SAMPLING", "1");
    //mTracePhotonPass.pProgram->addDefine("INFO_TEX_HEIGHT", std::to_string(mPhotonBufferHeight));
    mTracePhotonPass.pProgram->addDefine("TOTAL_PHOTON_COUNT", std::to_string(photonNumX * photonNumX));

    if (!mTracePhotonPass.pVars)
        prepareVars(mTracePhotonPass);
    FALCOR_ASSERT(mTracePhotonPass.pVars);

    auto var = mTracePhotonPass.pVars->getRootVar();
    var["PerFrame"]["gFrameCount"] = mFrameCount;
    var["PerFrame"]["gGlobalRadius"] = mGlobalRadius;
    var["PerFrame"]["gCausticRadius"] = mCausticInitRadius;
    var["PerFrame"]["gSeed"] = mUseFixedSeed ? mFixedSeed : mFrameCount;
    var["gSeeds"] = mSeeds;
    //var["PerFrame"]["gSeed"] = 0;
    if (mResetCB)
    {  
        var["CB"]["gUseAlphaTest"] = mUseAlphaTest;
        var["CB"]["gSpecRoughCutoff"] = 0.55f;
        var["CB"]["gDepth"] = mDepth;
    }
    FALCOR_ASSERT(mpEmissivePowerSampler);
    mpEmissivePowerSampler->bindShaderData(var["gEmissiveSampler"]);

    for (int i = 0; i < 2; i++)
    {
        auto& buffers = (i == 0) ? mCausticPhotonBuffers : mGlobalPhotonBuffers;
        pRenderContext->resourceBarrier(buffers.aabbs.get(), Resource::State::UnorderedAccess);
        var["gPhotonAABB"][i] = buffers.aabbs;
        var["gPhotonInfo"][i] = buffers.photonInfo;
    }

    var["gPhotonCounter"] = mPhotonCounter.counter;
    var["gPhotonImage"] = renderData.getTexture("PhotonImage");

    const uint2 targetDim = uint2(photonNumX, photonNumX); // trace 2^18 photons, may store 2^18 * (depth = 4) = 2^20 photons

    FALCOR_ASSERT(pRenderContext && mTracePhotonPass.pProgram && mTracePhotonPass.pVars);
    mpScene->raytrace(pRenderContext, mTracePhotonPass.pProgram.get(), mTracePhotonPass.pVars, uint3(targetDim, 1));

    for (int i = 0; i < 2; i++)
    {
        auto& buffers = (i == 0) ? mCausticPhotonBuffers : mGlobalPhotonBuffers;
        pRenderContext->uavBarrier(buffers.aabbs.get());
        //pRenderContext->uavBarrier(buffers.photonInfo.get());
    }
    pRenderContext->resourceBarrier(mCausticPhotonBuffers.aabbs.get(), Resource::State::NonPixelShader); // to be used for creating BLAS later
    pRenderContext->resourceBarrier(mGlobalPhotonBuffers.aabbs.get(), Resource::State::NonPixelShader);
}

void SPPM::collectPhotonPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "CollectPass");

    {
        // Pixel Debug
        const uint2 targetDim = renderData.getDefaultTextureDims();
        FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);
        mpPixelDebug->beginFrame(pRenderContext, targetDim);
    }
    assert(mpPixelDebug);

    mCollectPhotonPass.pProgram->addDefines(getValidResourceDefines(kInputChannels, renderData));
    mCollectPhotonPass.pProgram->addDefines(getValidResourceDefines(kOutputChannels, renderData));

    if (!mCollectPhotonPass.pVars)
        prepareVars(mCollectPhotonPass);
    FALCOR_ASSERT(mCollectPhotonPass.pVars);

    auto var = mCollectPhotonPass.pVars->getRootVar();

    var["PerFrame"]["gFrameCount"] = mFrameCount;
    var["PerFrame"]["gCausticRadius"] = mCausticRadius;
    var["PerFrame"]["gGlobalRadius"] = mGlobalRadius;
    var["PerFrame"]["gSeed"] = mUseFixedSeed ? 0 : mFrameCount;
    if (mResetCB)
    {
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
    for (int i = 0; i < 2; i++)
    {
        auto& buffer = (i == 0) ? mCausticPhotonBuffers : mGlobalPhotonBuffers;
        var["gPhotonAABB"][i] = buffer.aabbs;
        var["gPhotonInfo"][i] = buffer.photonInfo;
    }

    if (mpPixelDebug) mpPixelDebug->prepareProgram(mCollectPhotonPass.pProgram, var);

    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

    FALCOR_ASSERT(pRenderContext && mCollectPhotonPass.pProgram && mCollectPhotonPass.pVars);
    if (enableCollect) mpScene->raytrace(pRenderContext, mCollectPhotonPass.pProgram.get(), mCollectPhotonPass.pVars, uint3(targetDim, 1));

    if (mpPixelDebug) mpPixelDebug->endFrame(pRenderContext);
}

void SPPM::showASPass(RenderContext* pRenderContext, const RenderData& renderData)
{
    FALCOR_PROFILE(pRenderContext, "showASPass");
    mShowASPass.pProgram->addDefines(getValidResourceDefines(kInputChannels, renderData));
    mShowASPass.pProgram->addDefines(getValidResourceDefines(kOutputChannels, renderData));

    if (!mShowASPass.pVars)
        prepareVars(mShowASPass);

    auto var = mShowASPass.pVars->getRootVar();
    var["PerFrame"]["gGlobalRadius"] = mGlobalRadius;
    var["PerFrame"]["gCausticRadius"] = mCausticInitRadius;
    var["PerFrame"]["gCamPos"] = mpScene->getCamera()->getPosition();

    float3 cameraPos = mpScene->getCamera()->getPosition();
    printf("%.3lf %.3lf %.3lf\n", cameraPos.x, cameraPos.y, cameraPos.z);

    // input
    var["gVBuffer"] = renderData.getTexture("vbuffer");
    var["gViewWorld"] = renderData.getTexture("viewW");

    // output
    var["gPhotonImage"] = renderData.getTexture("PhotonImage");

    var["gPhotonAS"].setAccelerationStructure(mTlasInfo.falcorTlas);
    for (int i = 0; i < 2; i++)
    {
        auto& buffers = (i == 0) ? mCausticPhotonBuffers : mGlobalPhotonBuffers;
        var["gPhotonAABB"][i] = buffers.aabbs;
    }
    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

    mpScene->raytrace(pRenderContext, mShowASPass.pProgram.get(), mShowASPass.pVars, uint3(targetDim, 1));

}

void SPPM::buildBLAS(RenderContext* pRenderContext, PhotonBuffers& photonBuffers)
{
    //pRenderContext->resourceBarrier(photonBuffers.aabbs.get(), Resource::State::NonPixelShader);
    FALCOR_PROFILE(pRenderContext, "buildPhotonBlas");
    auto& blasInfo = photonBuffers.blasInfo;
    pRenderContext->uavBarrier(photonBuffers.blasScratch.get());

    blasInfo.geoDescs.content.proceduralAABBs.count = photonBuffers.maxPhotonCount; // update the count
    mPhotonASSizes.push_back(photonBuffers.maxPhotonCount);

    RtAccelerationStructure::BuildDesc asBuildDesc = {};
    asBuildDesc.inputs = blasInfo.inputs;
    asBuildDesc.scratchData = photonBuffers.blasScratch->getGpuAddress();
    asBuildDesc.dest = photonBuffers.falcorBlas.get();

    pRenderContext->buildAccelerationStructure(asBuildDesc, 0, nullptr);
    pRenderContext->uavBarrier(photonBuffers.blasBuffer.get()); // wait until the blas is built
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
    pRenderContext->uavBarrier(mTlasInfo.pTlasBuffer.get()); // wait until the tlas is built
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
            sbt->setHitGroup(0, 0, desc.addHitGroup("", "anyHit", "intersection"));
            mCollectPhotonPass.pProgram = Program::create(mpDevice, desc, mpScene->getSceneDefines());
        }
        {
            // show AS program
            ProgramDesc desc;
            desc.addShaderModules(mpScene->getShaderModules());
            desc.addShaderLibrary(kShowAS);
            desc.setMaxAttributeSize(kMaxAttributeSize);
            desc.setMaxPayloadSize(16);
            desc.setMaxTraceRecursionDepth(2);
            mShowASPass.pBindingTable = RtBindingTable::create(1, 1, mpScene->getGeometryCount());
            auto& sbt = mShowASPass.pBindingTable;
            sbt->setRayGen(desc.addRayGen("rayGen"));
            sbt->setMiss(0, desc.addMiss("miss"));
            sbt->setHitGroup(0, 0, desc.addHitGroup("photonASClosestHit", "", "intersection"));
            sbt->setHitGroup(0, 1, desc.addHitGroup("triangleClosestHit", "", ""));
            mShowASPass.pProgram = Program::create(mpDevice, desc, mpScene->getSceneDefines());
        }
    }

    // create seed buffer
    std::seed_seq seq{ time(0) };
    std::vector<uint32_t> cpuSeeds(1024 * 1024);
    seq.generate(cpuSeeds.begin(), cpuSeeds.end());
    mSeeds = mpDevice->createTexture2D(1024, 1024, ResourceFormat::R32Uint, 1, 1, cpuSeeds.data());
}
void SPPM::renderUI(Gui::Widgets& widget)
{
    // render debug UI
    widget.text("Caustic Photons: " + std::to_string(mPhotonCounts[0]) + " / " + std::to_string(mPhotonASSizes[0]));
    widget.text("Global Photons: " + std::to_string(mPhotonCounts[1]) + " / " + std::to_string(mPhotonASSizes[1]));
    widget.tooltip("Photons for current Iteration / Build Size Acceleration Structure");
    widget.text("Current Global Radius: " + std::to_string(mGlobalRadius));
    widget.text("Current Caustic Radius: " + std::to_string(mCausticRadius));

    bool dirty = false;

    dirty |= widget.var("Photon Bounces", mDepth, 0u, 1u << 16);
    dirty |= widget.checkbox("Enable Collect", enableCollect);
    dirty |= widget.var("Photon Number", photonNumX, 0u, 1u << 16);
    
    widget.var("Max Photon Count", mMaxPhotonCount, 0u, 1u << 16);
    updateMaxPhotonCount = widget.button("Apply");
    dirty |= updateMaxPhotonCount;
    if (updateMaxPhotonCount)
        photonNumX = std::min(photonNumX, mMaxPhotonCount / 2); // assuming maximum depth is 4


    if (auto g = widget.group("Debugging"))
    {
        dirty |= g.checkbox("Use fixed seed", mUseFixedSeed);
        g.tooltip("Forces a fixed random seed for each frame.\n\n"
            "This should produce exactly the same image each frame, which can be useful for debugging.");
        if (mUseFixedSeed)
        {
            dirty |= g.var("Seed", mFixedSeed);
        }
        mpPixelDebug->renderUI(g);
    }
    if (dirty)
    {
        mOptionChanged = true;
    }
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

void SPPM::preparePhotonBuffers(PhotonBuffers& photonBuffers)
{
    {
        uint maxPhotonCount = mMaxPhotonCount * mMaxPhotonCount;

        photonBuffers.aabbs = mpDevice->createStructuredBuffer(sizeof(RtAABB), maxPhotonCount);
        photonBuffers.aabbs->setName("photon aabbs");
        FALCOR_ASSERT(photonBuffers.aabbs);

        photonBuffers.photonInfo = mpDevice->createStructuredBuffer(sizeof(PhotonInfo), maxPhotonCount);
        photonBuffers.photonInfo->setName("photon info");
        FALCOR_ASSERT(photonBuffers.photonInfo);
    }
}

void SPPM::prepareTLAS(RenderContext* pRenderContext)
{
    {
        RtInstanceDesc causticDesc = {};
        causticDesc.accelerationStructure = mCausticPhotonBuffers.blasBuffer->getGpuAddress();
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
        globalDesc.instanceContributionToHitGroupIndex = 0; // two instances use the same hit group
        float4x4 tranform;
        std::memcpy(globalDesc.transform, &tranform, sizeof(globalDesc.transform));
        photonInstanceDescs.push_back(globalDesc);
    }
    RtAccelerationStructureBuildInputs inputs = {};
    inputs.kind = RtAccelerationStructureKind::TopLevel;
    inputs.descCount = (uint32_t)photonInstanceDescs.size();
    inputs.flags = RtAccelerationStructureBuildFlags::PreferFastTrace;

    // copy data from upload buffer to device local buffer
    ref<Buffer> temp_buffer = mpDevice->createBuffer((uint32_t)photonInstanceDescs.size() * sizeof(RtInstanceDesc), ResourceBindFlags::None, MemoryType::Upload, photonInstanceDescs.data());
    mTlasInfo.pInstanceDescs = mpDevice->createBuffer((uint32_t)photonInstanceDescs.size() * sizeof(RtInstanceDesc), ResourceBindFlags::None, MemoryType::DeviceLocal, nullptr);
    pRenderContext->copyBufferRegion(mTlasInfo.pInstanceDescs.get(), 0, temp_buffer.get(), 0, temp_buffer->getSize());

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
    photonBuffers.blasBuffer.reset();
    photonBuffers.blasScratch.reset();

    auto& blasInfo = photonBuffers.blasInfo;
    auto& desc = blasInfo.geoDescs;
    desc.type = RtGeometryType::ProcedurePrimitives;
    desc.flags = RtGeometryFlags::NoDuplicateAnyHitInvocation; // Each photon appears exactly once in anyhit shader
    //desc.flags = RtGeometryFlags::None;
    desc.content.proceduralAABBs.count = mMaxPhotonCount * mMaxPhotonCount; // create a large buffer for pre built
    desc.content.proceduralAABBs.data = photonBuffers.aabbs->getGpuAddress();
    desc.content.proceduralAABBs.stride = sizeof(RtAABB);

    auto& inputs = blasInfo.inputs;
    inputs.kind = RtAccelerationStructureKind::BottomLevel;
    inputs.descCount = 1;
    inputs.geometryDescs = &desc;
    inputs.flags = RtAccelerationStructureBuildFlags::PreferFastTrace; // Because we always need to enumerate all leaves, we choose fast build (maybe LBVH?)
    

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
