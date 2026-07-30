from dataclasses import dataclass
import glm

@dataclass
class SpectrumParameters:
    NoiseSeed : int
    DirectionalSpreadingMode : int
    WindSpeed : float
    Fetch : float
    Depth : float
    SwellAmount : float
    Alignment : float
    LocalWindDirection : float
    SwellDirection : float
    FadeLength : float
    PeakingFactor : float

@dataclass
class CascadeParameters:
    LengthScales : glm.vec4
    CutoffsHigh : glm.vec4
    CutoffsLow : glm.vec4



