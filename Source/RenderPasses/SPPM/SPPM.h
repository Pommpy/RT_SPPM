#pragma once
#include "Falcor.h"
#include "RenderGraph/RenderPass.h"
#include "Utils/Sampling/SampleGenerator.h"
#include "Rendering/Lights/LightBVHSampler.h"
#include "Rendering/Lights/EmissivePowerSampler.h"
#include "Utils/Debug/PixelDebug.h"
#include "Rendering/Utils/PixelStats.h"
#include "Core/API/RtAccelerationStructure.h"
#include "Core/API/Device.h"

using namespace Falcor;

struct SubPass
{
    ref<Program> pProgram;
    ref<RtBindingTable> pBindingTable;
    ref<RtProgramVars> pVars;
    void init()
    {
        pProgram = nullptr;
        pBindingTable = nullptr;
        pVars = nullptr;
    }
};
struct BlasInfo
{
    RtAccelerationStructurePrebuildInfo prebuildInfo;
    RtAccelerationStructureBuildInputs inputs;
    RtGeometryDesc geoDescs;
    uint64_t blasSize = 0;
    uint64_t scratchBufferSize = 0;
};
struct TlasInfo
{
    RtAccelerationStructureBuildInputs inputs;
    RtAccelerationStructurePrebuildInfo prebuildInfo;
    ref<Buffer> pInstanceDescs;
    ref<Buffer> pTlasBuffer;
    ref<Buffer> pScratch;
    ref<RtAccelerationStructure> falcorTlas;
};
struct PhotonBuffers
{
    uint size;
    ref<Texture> flux;
    ref<Texture> dir;
    ref<Buffer> aabbs; // aabb structured buffer built within photon trace pass, used for building blas
    ref<Buffer> blasScratch;
    ref<Buffer> blasBuffer;
    ref<RtAccelerationStructure> falcorBlas;
    BlasInfo blasInfo;
};
struct Timer
{
    std::chrono::time_point<std::chrono::steady_clock> startTime;
    std::vector<double> timesList;
};
class SPPM : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(SPPM, "SPPM", "My SPPM Impl.");
    static ref<SPPM> create(ref<Device> pDevice, const Properties& props)
    {
        return make_ref<SPPM>(pDevice, props);
    }
    SPPM(ref<Device> pDevice, const Properties& props);
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void renderUI(Gui::Widgets& widget);
    virtual void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;
    virtual bool onMouseEvent(const MouseEvent& mouseEvent) override {
        //return mpPixelDebug->onMouseEvent(mouseEvent);
        return false;
    }
    virtual bool onKeyEvent(const KeyboardEvent& keyEvent) override {
        return false;
    }
    void recordTimer();


    // Functions for photon mapping
    void preparePhotonBuffers(PhotonBuffers& photonBuffers);
    void preparePhotonTextures(PhotonBuffers& photonBuffers);
    void prepareBLAS(PhotonBuffers& photonBuffers);
    void prepareTLAS(RenderContext* pRenderContext);
    void tracePhotonPass(RenderContext* pRenderContext, const RenderData& renderData);
    void buildBLAS(RenderContext* pRenderContext, PhotonBuffers& photonBuffers);
    void buildTLAS(RenderContext* pRenderContext);
    void collectPhotonPass(RenderContext* pRenderContext, const RenderData& renderData);
    void resetSPPM();
    void prepareVars(SubPass& pass);

    // Scene settings
    ref<Scene> mpScene;
    uint mFrameCount = 0;
    bool mOptionChanged = false;
    bool mResetIteration = false;
    bool mResetCB = false;
    bool mResetTimer = false;
    bool mUseTimer = true;

    // UI settings
    bool mUseFixedSeed = false;
    bool mUseAlphaTest = true;
    uint mDepth = 4;

    // Photon mapping settings
    const float mCausticInitRadius = 0.01f; // Initial radius
    const float mGlobalInitRadius = 0.05f;
    const float mSPPMAlpha = 0.7f;
    const float kMinPhotonRadius = 0.00001f;
    float mCausticRadius = 0.01f; // current radius
    float mGlobalRadius = 0.05f;
    bool mResizePhotonBuffer = true; 
    bool mRebuildAS = true;
    bool mCreateBuffer = true; // need to create buffers at frame 0
    uint mPhotonBufferWidth = 1024;
    uint mPhotonBufferHeight = 1024; // which means we store 1024 * 1024 = 2^20 total photons each iteration, this value can be updated through GUI
    uint32_t mCausticPhotonCount = 0;
    uint32_t mGlobalPhotonCount = 0;

    // Timer
    Timer mTimer;

    // Photon Mapping Subpasses
    SubPass mTracePhotonPass;
    SubPass mCollectPhotonPass;

    // Photon Buffers
    PhotonBuffers mCausticPhotonBuffers;
    PhotonBuffers mGlobalPhotonBuffers;
    ref<Buffer> mPhotonCounter; // used to accumulate photon count in photon generation pass
    ref<Buffer> mPhotonCounterReset;
    std::vector<RtInstanceDesc> photonInstanceDescs;
    TlasInfo mTlasInfo;

    ref<SampleGenerator> mpSampleGenerator;
    std::unique_ptr<EmissivePowerSampler> mpEmissivePowerSampler; // Sample emissive lights based on their flux
    // TO DO:
    // UI Control:
    // Photon Number, BLAS Build
};
