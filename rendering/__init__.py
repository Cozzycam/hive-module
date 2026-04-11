"""Shared visual definitions — palette and sprite data.

Lives outside emulator/ because the same data feeds ESP32 firmware
eventually. Keep this package free of pygame imports for that reason;
the emulator converts sprite data into pygame Surfaces at load time.
"""
