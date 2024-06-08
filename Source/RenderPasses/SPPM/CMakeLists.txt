add_plugin(SPPM)

target_sources(SPPM PRIVATE
    SPPM.cpp
    SPPM.h
    TracePhoton.rt.slang
    CollectPhoton.rt.slang
    ShowAS.rt.slang
)

target_copy_shaders(SPPM RenderPasses/SPPM)

target_source_group(SPPM "RenderPasses")
