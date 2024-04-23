#pragma once
#include "Falcor.h"
#include "RenderGraph/RenderPass.h"
#include "Utils/Sampling/SampleGenerator.h"
#include "Rendering/Lights/LightBVHSampler.h"
#include "Rendering/Lights/EmissivePowerSampler.h"
#include "Utils/Debug/PixelDebug.h"
#include "Rendering/Utils/PixelStats.h"
#include "Rendering/RTXDI/RTXDI.h"

using namespace Falcor;

/*
* MyPathTracer: A copy of MinimalPathTracer to enhance understanding of Falcor pipeline
*/
class MyPathTracer : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(MyPathTracer, "MyPathTracer", "My Path Tracer.");

    static ref<MyPathTracer> create(ref<Device> pDevice, const Properties& props)
    {
        return make_ref<MyPathTracer>(pDevice, props);
    }

    MyPathTracer(ref<Device> pDevice, const Properties& props);
    virtual Properties getProperties() const override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void renderUI(Gui::Widgets& widget);
    virtual void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;
    virtual bool onMouseEvent(const MouseEvent& mouseEvent) override {
        return mpPixelDebug->onMouseEvent(mouseEvent);
        return false;
    }
    virtual bool onKeyEvent(const KeyboardEvent& keyEvent) override {
        return false;
    }
    PixelStats& getPixelStats() { return *mpPixelStats; }

    void prepareRTXDI(RenderContext* pRenderContext);
    //void reset();

    //static void registerBindings(pybind11::module& m);

private:
    void parseProperties(const Properties& props);
    void prepareVars();

    // Internal States
    // 
    // current scene
    ref<Scene> mpScene;
    // GPU sample generator
    ref<SampleGenerator> mpSampleGenerator;
    // Emissive Sampler
    std::unique_ptr<EmissiveLightSampler> mpEmissiveSampler;    ///< Emissive light sampler or nullptr if not used.
    RTXDI::Options mRTXDIOptions;
    std::unique_ptr<RTXDI> mpRTXDI;
    std::unique_ptr<PixelStats> mpPixelStats; // collecting pixel stats
    std::unique_ptr<PixelDebug> mpPixelDebug; // print in shaders

    // Max number of indirect bounces
    uint mMaxBounces = 3;

    // Compute direct illumination
    bool mComputeDirect = true;

    // Importance Sample materials
    bool mUseImportanceSampling = true;
    bool mUseMIS = true;
    bool mUseNEE = true;
    bool mUseRTXDI = true;

    bool mUseFixedSeed = false;
    uint mFixedSeed = 1;

    // Runtime Data
    // Frame count since scene was loaded
    uint mFrameCount = 0;
    bool mOptionChanged = false;

    // Ray tracing program
    struct
    {
        ref<Program> pProgram;
        ref<RtBindingTable> pBindingTable;
        ref<RtProgramVars> pVars;
    }mTracer;
};
