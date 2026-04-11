"""Pygame display shell that emulates the physical module network.

All pygame imports live in this package. The sim/ package must never
import from here — the rule is one-way: emulator reads sim state.
"""
