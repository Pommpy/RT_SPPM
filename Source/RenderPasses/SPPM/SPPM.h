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
struct PhotonInfo
{
    float3 flux;
    float3 dir;
};

struct PhotonCounter {
    ref<Buffer> counter;
    ref<Buffer> reset;
    ref<Buffer> cpuReadback;
};

struct PhotonBuffers
{
    uint maxPhotonCount;
    ref<Buffer> photonInfo; // packed flux and dir
    // ref<Texture> flux;
    // ref<Texture> dir;
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
    void resetPhotonCounter(RenderContext* pRenderContext);
    void prepareBLAS(PhotonBuffers& photonBuffers);
    void prepareTLAS(RenderContext* pRenderContext);
    void tracePhotonPass(RenderContext* pRenderContext, const RenderData& renderData);
    void buildBLAS(RenderContext* pRenderContext, PhotonBuffers& photonBuffers);
    void buildTLAS(RenderContext* pRenderContext);
    void collectPhotonPass(RenderContext* pRenderContext, const RenderData& renderData);
    void showASPass(RenderContext* pRenderContext, const RenderData& renderData);
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
    uint mFixedSeed = 0;
    bool mUseAlphaTest = true;
    uint mDepth = 4;
    uint photonNumX = 512;
    bool numPhotonChanged = true; // for the fisrt frame

    // Photon mapping settings
    const float mCausticInitRadius = 0.01f; // Initial radius
    const float mGlobalInitRadius = 0.05f;
    const float mSPPMAlpha = 0.7f;
    const float kMinPhotonRadius = 0.00001f;
    float mCausticRadius = 0.005f; // current radius
    float mGlobalRadius = 0.01f;
    bool mResizePhotonBuffer = true; 
    bool mRebuildAS = true;
    bool mCreateBuffer = true; // need to create buffers at frame 0

    bool updateMaxPhotonCount = false;
    uint mMaxPhotonCount = 1024;

    float photonASScale = 1.1f;
    std::vector<uint32_t> mPhotonCounts = { 1, 1 };
    std::vector<uint32_t> mPhotonASSizes = { 1, 1 };

    bool enableCollect = true;

    // Timer
    Timer mTimer;

    // Photon Mapping Subpasses
    SubPass mTracePhotonPass;
    SubPass mCollectPhotonPass;
    SubPass mShowASPass;

    // Photon Buffers
    PhotonBuffers mCausticPhotonBuffers;
    PhotonBuffers mGlobalPhotonBuffers;
    PhotonCounter mPhotonCounter;

    // ref<Buffer> mPhotonCounter; // used to accumulate photon count in photon generation pass
    // ref<Buffer> mPhotonCounterReset;
    ref<Texture> mSeeds;
    std::vector<RtInstanceDesc> photonInstanceDescs;
    TlasInfo mTlasInfo;

    ref<SampleGenerator> mpSampleGenerator;
    std::unique_ptr<EmissivePowerSampler> mpEmissivePowerSampler; // Sample emissive lights based on their flux

    std::unique_ptr<PixelStats> mpPixelStats; // collecting pixel stats
    std::unique_ptr<PixelDebug> mpPixelDebug; // print in shaders

    // TO DO:
    // Pixel Debug
    // UI Control:
    // Photon Number, BLAS Build
};
