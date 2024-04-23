from falcor import *

def render_graph_SPPM():
    g = RenderGraph('SPPM')
    VBufferRT = createPass('VBufferRT', {'samplePattern': 'Stratified', 'sampleCount': 1})
    g.addPass(VBufferRT, 'VBufferRT')
    SPPM = createPass('SPPM')
    g.addPass(SPPM, 'SPPM')
    ToneMapper = createPass('ToneMapper', {'autoExposure': False, 'exposureCompensation': 0.0})
    g.addPass(ToneMapper, 'ToneMapper')
    g.addEdge('SPPM.PhotonImage', 'ToneMapper.src')
    g.addEdge('VBufferRT.vbuffer', 'SPPM.vbuffer')
    g.addEdge('VBufferRT.viewW', 'SPPM.viewW')
    g.markOutput('ToneMapper.dst')
    return g

RTPM = render_graph_SPPM()
try: m.addGraph(RTPM)
except NameError: None
