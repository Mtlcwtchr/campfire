"""Hemisphere layout shared by asset metadata and the engine's view mapping.

Orthonormal frames: right cross up == -eye (screen convention); centred sphere.
Keep in sync with engine/render/hemisphere_impostor.hpp and its GPU parity tests.
"""
import math
import numpy as np

LAYOUT = 'rings-8-8-4-1-v1'
VIEW_COUNT = 21


def basis(view):
    if not 0 <= view < VIEW_COUNT:
        raise ValueError('invalid hemisphere view')
    ring = 0 if view < 8 else 1 if view < 16 else 2 if view < 20 else 3
    first, count, elevation = ((0,8,0),(8,8,45),(16,4,70),(20,1,90))[ring]
    angle = (view-first)*math.tau/count
    e = math.radians(elevation)
    c,s,ce,se = math.cos(angle),math.sin(angle),math.cos(e),math.sin(e)
    return np.array([[c,s,0],[s*se,-c*se,ce],[-s*ce,c*ce,se]], dtype=float)
