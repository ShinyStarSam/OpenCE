"""Runs kev.serve with its CUDA graph buffers sized for one player.

kev.cuda_graphs reserves room for a busy server: a bank of 16 cached states of
4,096 tokens each and 32,768 tokens of row-pass buffers. Kevin asks about one
game state of a few hundred tokens at a time, so this shrinks those limits
before the server starts. Kev's own code is unchanged. Arguments are passed
to kev.serve; KEV_GRAPH_STATES, KEV_BANK_WIDTH and KEV_GRAPH_TOKENS override
the sizes.
"""

import os
import runpy

import kev.cuda_graphs as graphs

graphs.GRAPH_STATES = int(os.environ.get("KEV_GRAPH_STATES", "2"))
graphs.BANK_WIDTH = int(os.environ.get("KEV_BANK_WIDTH", "2048"))
graphs.GRAPH_TOKENS = int(os.environ.get("KEV_GRAPH_TOKENS", "8192"))
runpy.run_module("kev.serve", run_name="__main__", alter_sys=True)
