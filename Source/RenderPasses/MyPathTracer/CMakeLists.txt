add_plugin(MyPathTracer)

target_sources(MyPathTracer PRIVATE
    MyPathTracer.cpp
    MyPathTracer.h
    MyPathTracer.rt.slang
)

target_copy_shaders(MyPathTracer RenderPasses/MyPathTracer)

target_source_group(MyPathTracer "RenderPasses")
