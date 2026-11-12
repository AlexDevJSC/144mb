/* SPDX-License-Identifier: MIT */
/* Signature list for the FluidSynth 2.x public ABI with SONAME 3. */
FLUID(void *, new_fluid_settings, (void), ())
FLUID_VOID(delete_fluid_settings, (void *settings), (settings))
FLUID(int, fluid_settings_setnum, (void *settings, const char *key, double value), (settings, key, value))
FLUID(int, fluid_settings_setint, (void *settings, const char *key, int value), (settings, key, value))
FLUID(void *, new_fluid_synth, (void *settings), (settings))
FLUID_VOID(delete_fluid_synth, (void *synth), (synth))
FLUID(int, fluid_synth_noteon, (void *synth, int channel, int key, int velocity), (synth, channel, key, velocity))
FLUID(int, fluid_synth_noteoff, (void *synth, int channel, int key), (synth, channel, key))
FLUID(int, fluid_synth_cc, (void *synth, int channel, int control, int value), (synth, channel, control, value))
FLUID(int, fluid_synth_program_change, (void *synth, int channel, int program), (synth, channel, program))
FLUID(int, fluid_synth_bank_select, (void *synth, int channel, int bank), (synth, channel, bank))
FLUID(int, fluid_synth_system_reset, (void *synth), (synth))
FLUID(int, fluid_synth_all_notes_off, (void *synth, int channel), (synth, channel))
FLUID(int, fluid_synth_all_sounds_off, (void *synth, int channel), (synth, channel))
FLUID(int, fluid_synth_set_gen, (void *synth, int channel, int parameter, float value), (synth, channel, parameter, value))
FLUID(int, fluid_synth_sfunload, (void *synth, int soundfont, int reset), (synth, soundfont, reset))
FLUID(int, fluid_synth_set_reverb, (void *synth, double room, double damping, double width, double level),
      (synth, room, damping, width, level))
FLUID(int, fluid_synth_set_chorus, (void *synth, int voices, double level, double speed, double depth, int type),
      (synth, voices, level, speed, depth, type))
FLUID_VOID(fluid_synth_set_gain, (void *synth, float gain), (synth, gain))
FLUID(int, fluid_synth_get_active_voice_count, (void *synth), (synth))
FLUID(int, fluid_synth_write_float,
      (void *synth, int frames, void *left, int left_offset, int left_step,
       void *right, int right_offset, int right_step),
      (synth, frames, left, left_offset, left_step, right, right_offset, right_step))
