#include "MyPathTracer.h"
#include "RenderGraph/RenderPassHelpers.h"
#include "RenderGraph/RenderPassStandardFlags.h"

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry & registry)
{
    registry.registerClass<RenderPass, MyPathTracer>();
    //ScriptBindings::registerBinding(MyPathTracer::registerBindings);
}

//void MyPathTracer::registerBindings(pybind11::module& m)
//{
//    pybind11::class_<MyPathTracer, RenderPass, ref<MyPathTracer>> pass(m, "MyPathTracer");
//}

namespace
{
const char kShaderFile[] = "RenderPasses/MyPathTracer/MyPathTracer.rt.slang";

// Ray tracing settings
const uint32_t kMaxPayloadSizeBytes = 512u;
const uint32_t kMaxRecursionDepth = 2u;

const std::string kInputViewDir = "viewW";
const std::string kInputMotionVectors = "mvec";

const ChannelList kInputChannels =
{
    {"vbuffer",             "gVBuffer",                 "V Buffer to get the intersected triangle",         false},
    {"viewW",               "gViewWorld",               "World View Direction",                             false},
    {"thp",                 "gThp",                     "Throughput",                                       false},
    {"emissive",            "gEmissive",                "Emissive",                                         false},
};

const ChannelList kOutputChannels =
{
    { "PhotonImage",          "gPhotonImage",               "An image that shows the caustics and indirect light from global photons" , false , ResourceFormat::RGBA32Float }
};

const char kMaxBounces[] = "maxBounces";
const char kComputeDirect[] = "computeDirect";
const char kUseImportanceSampling[] = "useImmportanceSampling";
}

MyPathTracer::MyPathTracer(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice)
{
    parseProperties(props);

    mpSampleGenerator = SampleGenerator::create(mpDevice, SAMPLE_GENERATOR_UNIFORM);

    mpPixelStats = std::make_unique<PixelStats>(mpDevice);
    mpPixelDebug = std::make_unique<PixelDebug>(mpDevice);
    FALCOR_ASSERT(mpSampleGenerator);
}

// Parse configurations
void MyPathTracer::parseProperties(const Properties& props)
{
    for (const auto& [key, value] : props)
    {
        if (key == kMaxBounces)
        {
            mMaxBounces = value;
        }
        else if (key == kComputeDirect)
        {
            mComputeDirect = value;
        }
        else if (key == kUseImportanceSampling)
        {
            mUseImportanceSampling = value;
        }
        else
        {
            logWarning("Unknown Property '{}' in MyPathTracer.", key);
        }
    }
}

Properties MyPathTracer::getProperties() const
{
    // This is for pybind, not used now
    Properties props;
    props[kMaxBounces] = mMaxBounces;
    props[kComputeDirect] = mComputeDirect;
    props[kUseImportanceSampling] = mUseImportanceSampling;
    return props;
}

RenderPassReflection MyPathTracer::reflect(const CompileData& compileData)
{
    RenderPassReflection reflector;

    // Define input/output channels
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels);

    return reflector;
}

void MyPathTracer::prepareRTXDI(RenderContext* pRenderContext)
{
    if (mUseRTXDI)
    {
        if (!mpRTXDI) mpRTXDI = std::make_unique<RTXDI>(mpScene, mRTXDIOptions);
    }
    else
    {
        mpRTXDI = nullptr;
    }
}

void MyPathTracer::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    auto& dict = renderData.getDictionary();
    if (mOptionChanged)
    {
        // We need to refresh if options affecting output are changed
        auto flags = dict.getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
        dict[Falcor::kRenderPassRefreshFlags] = flags | Falcor::RenderPassRefreshFlags::RenderOptionsChanged;
        mOptionChanged = false;
    }

    // If we have no scene, clear output and return
    if (!mpScene)
    {
        for (auto it : kOutputChannels)
        {
            Texture* pDst = renderData.getTexture(it.name).get();
            if (pDst)
            {
                pRenderContext->clearTexture(pDst);
            }
        }
        return;
    }

    if (is_set(mpScene->getUpdates(), Scene::UpdateFlags::RecompileNeeded) ||
        is_set(mpScene->getUpdates(), Scene::UpdateFlags::GeometryChanged))
    {
        FALCOR_THROW("This render pass does not support scene changes that require shader recompilation.");
    }

    {
        // Pixel Debug
        const uint2 targetDim = renderData.getDefaultTextureDims();
        FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

        // RTXDI prepare
        prepareRTXDI(pRenderContext);
        if (mpRTXDI) mpRTXDI->beginFrame(pRenderContext, targetDim);

        mpPixelDebug->beginFrame(pRenderContext, targetDim);

    }


    // Request the light collection if emissive lights are enabled
    if (mpScene->getRenderSettings().useEmissiveLights)
    {
        mpScene->getLightCollection(pRenderContext);
    }

    // Configure depth-of-field
    const bool useDOF = mpScene->getCamera()->getApertureRadius() > 0.f;
    if (useDOF && renderData[kInputViewDir] == nullptr)
    {
        logWarning("DOF requires '{}' input. Expect incorrect shading.", kInputViewDir);
    }


    // Specialize program.
    mTracer.pProgram->addDefine("MAX_BOUNCES", std::to_string(mMaxBounces));
    mTracer.pProgram->addDefine("COMPUTE_DIRECT", mComputeDirect ? "1" : "0");
    mTracer.pProgram->addDefine("USE_IMPORTANCE_SAMPLING", mUseImportanceSampling ? "1" : "0");
    mTracer.pProgram->addDefine("USE_ANALYTIC_LIGHTS", mpScene->useAnalyticLights() ? "1" : "0");
    mTracer.pProgram->addDefine("USE_EMISSIVE_LIGHTS", mpScene->useEmissiveLights() ? "1" : "0");
    mTracer.pProgram->addDefine("USE_ENV_LIGHT", mpScene->useEnvLight() ? "1" : "0");
    mTracer.pProgram->addDefine("USE_ENV_BACKGROUND", mpScene->useEnvBackground() ? "1" : "0");


    // MIS config
    mTracer.pProgram->addDefine("USE_MIS", mUseMIS ? "1" : "0");
    mTracer.pProgram->addDefine("USE_NEE", mUseNEE ? "1" : "0");

    // RTXDI
    mTracer.pProgram->addDefine("USE_RTXDI", mUseRTXDI ? "1" : "0");

    mTracer.pProgram->addDefines(getValidResourceDefines(kInputChannels, renderData));
    mTracer.pProgram->addDefines(getValidResourceDefines(kOutputChannels, renderData));
    if (mpRTXDI) mTracer.pProgram->addDefines(mpRTXDI->getDefines());


    if (!mTracer.pVars)
    {
        prepareVars();
    }
    FALCOR_ASSERT(mTracer.pVars);

    auto var = mTracer.pVars->getRootVar();

    // bind shader data
    var["CB"]["gFrameCount"] = mFrameCount;
    var["CB"]["gPRNGDimension"] = dict.keyExists(kRenderPassPRNGDimension) ? dict[kRenderPassPRNGDimension] : 0u;
    // should bind path tracer params here
    var["CB"]["gSeed"] = mUseFixedSeed ? mFixedSeed : mFrameCount;

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

    // Update RTXDI.
    if (mpRTXDI)
    {
        //const auto& pMotionVectors = renderData.getTexture(kInputMotionVectors);
        mpRTXDI->update(pRenderContext, nullptr);
    }

    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

    mpPixelDebug->prepareProgram(mTracer.pProgram, var);
    if (mpRTXDI) mpRTXDI->bindShaderData(var);

    mpScene->raytrace(pRenderContext, mTracer.pProgram.get(), mTracer.pVars, uint3(targetDim, 1));


    // end frame
    if(mpPixelDebug) mpPixelDebug->endFrame(pRenderContext);

    if (mpRTXDI) mpRTXDI->endFrame(pRenderContext);
    mFrameCount++;
}

void MyPathTracer::renderUI(Gui::Widgets& widget)
{
    bool dirty = false;

    dirty |= widget.var("Max bounces", mMaxBounces, 0u, 1u << 16);
    widget.tooltip("Maximum path length for indirect.\n 0 = direct only\n 1 = one bounce indirect etc.", true);

    dirty |= widget.checkbox("Evaluate direct illumination", mComputeDirect);
    widget.tooltip("Compute direct illumination.\nIf disabled only indirect illumination is computed. (when max bounce > 0)", true);

    dirty |= widget.checkbox("Use importance sampling", mUseImportanceSampling);

    dirty |= widget.checkbox("Use MIS", mUseMIS);
    dirty |= widget.checkbox("Use NEE", mUseNEE);

    if (auto group = widget.group("RTXDI"))
    {
        dirty |= widget.checkbox("Enabled", mUseRTXDI);
        widget.tooltip("Use RTXDI for direct illumination.");
        if (mpRTXDI) dirty |= mpRTXDI->renderUI(group);
    }
    //widget.tooltip("U")

    // Render stats UI
    //if(auto g = widget.group("Statistics"))
    //{
    //    mpPixelStats->renderUI(g);
    //}

    // render debug UI
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

void MyPathTracer::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    // clear data for previous frame
    // After changing scene, the program should be recreated
    mTracer.pProgram = nullptr;
    mTracer.pBindingTable = nullptr;
    mTracer.pVars = nullptr;
    mFrameCount = 0;

    // set new scene
    mpScene = pScene;

    if (mpScene)
    {
        if (pScene->hasGeometryType(Scene::GeometryType::Custom))
        {
            logWarning("MyPathTracer: This render pass does not support custom primitives");
        }

        ProgramDesc desc;
        desc.addShaderModules(mpScene->getShaderModules());
        desc.addShaderLibrary(kShaderFile);
        desc.setMaxPayloadSize(kMaxPayloadSizeBytes);
        desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
        desc.setMaxTraceRecursionDepth(kMaxRecursionDepth);

        mTracer.pBindingTable = RtBindingTable::create(2, 2, mpScene->getGeometryCount());
        auto& sbt = mTracer.pBindingTable;
        sbt->setRayGen(desc.addRayGen("rayGen"));
        sbt->setMiss(0, desc.addMiss("scatterMiss"));
        sbt->setMiss(1, desc.addMiss("shadowMiss"));

        if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
        {
            sbt->setHitGroup(
                0,
                mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh),
                desc.addHitGroup("scatterTriangleMeshClosestHit", "scatterTriangleMeshAnyHit")
            );
            sbt->setHitGroup(
                1, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("", "shadowTriangleMeshAnyHit")
            );
        }

        mTracer.pProgram = Program::create(mpDevice, desc, mpScene->getSceneDefines());
    }
}

void MyPathTracer::prepareVars()
{
    FALCOR_ASSERT(mpScene);
    FALCOR_ASSERT(mTracer.pProgram);

    mTracer.pProgram->addDefines(mpSampleGenerator->getDefines());
    mTracer.pProgram->setTypeConformances(mpScene->getTypeConformances());

    // May trigger shader compilation
    mTracer.pVars = RtProgramVars::create(mpDevice, mTracer.pProgram, mTracer.pBindingTable);

    // Bind utility class
    auto var = mTracer.pVars->getRootVar();
    mpSampleGenerator->bindShaderData(var);
}
