EAR / BONES MODKIT ABI 1 — Motor de audio version funcional 0.4.0

Este directorio pertenece a Ear. Bones sólo compila modkit.c, que incluye
la síntesis, composición, materiales y salida WinMM como unidad de compilación.
NO añadir ear_output.c ni ear_core/*.c a la compilación por separado.

Público para Skin: ear_audio_control.h (sin UI ni dependencias de Bones)
Punto de entrada: bones_mod_ear_query(BONES_MOD_ABI, BonesModDescriptor*)
Rol: BONES_ROLE_EAR | permisos: READ_ACTORS, READ_TIME | sin dependencias.

Al iniciar genera música de mazmorra. Si falla WinMM, Bones puede arrancar
sin audio: consultar error en EarAudioStatus y usar retry explícito. El host
puede llamar ear_audio_pump una vez por frame; after_tick bombea normalmente.
JUMP/LAND disparan SFX una sola vez por fuente/tipo/tick en un buffer de 16.

Documentación: Ear/docs/CONTRATO_CONTROL_AUDIO_SKIN_v1.txt y
Ear/docs/NOTAS_INTEGRACION_BONES_WINDOWS_v1.txt.

Prueba real WinMM: Ear/scripts/test_windows_audio.bat desde VS x64 prompt.
No se ha verificado hardware Windows ni tamaño PE en Linux.
