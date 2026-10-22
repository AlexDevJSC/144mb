# ACTA DE PLAYTEST — EAR 0.4.1 / BONES 0.4.0

**Fecha del documento:** 21/10/2026
**Estado de aceptación real con hardware:** PENDIENTE DE EJECUCIÓN (NO inventar resultados).
**PR:** https://github.com/AlexDevJSC/144mb/pull/13
**Evidencia CI Windows sin salida:** https://github.com/AlexDevJSC/144mb/actions/runs/39608044774
**Pequeña ampliación de pruebas:** https://github.com/AlexDevJSC/144mb/tree/ear/audio-v0.4-winmm-controls/Ear/tools

## 1. Resultados verificables antes del playtest

Windows Server 2022 / Visual Studio MSVC en GitHub Actions:
- Build Ear + Bones: PASS.
- PE baseline: 206.848 bytes; con Ear: 222.720 bytes; delta +15.872 bytes.
- No había dispositivo de salida: EarAudioError=4 / MMRESULT=2.
- No se probó audibilidad, buffers completados con salida real ni limpieza tras abrir dispositivo.

## 2. Preparación manual — Windows con altavoces o auriculares funcionales

Registrar antes de empezar:
- Fecha/hora local: __________
- Operador: __________
- Windows/arquitectura: __________
- Dispositivo de reproducción y controlador: __________
- Commit exacto de la PR: __________
- Volumen SO/salida predeterminada: __________

Clonar o actualizar la rama `ear/audio-v0.4-winmm-controls`.
Abrir **x64 Native Tools Command Prompt** de Visual Studio.
Ejecutar desde la raíz del repositorio:

```bat
Ear\scripts\test_windows_audio.bat
```

Si el dispositivo no abre, la prueba informa la causa y falla (no declarar OK).
Si abre, deja reproducir el audio 5 s. Se oyen música de mazmorra, salto (aprox.
1 s), aterrizaje (aprox. 1,6 s), variación de música (aprox. 1,8 s),
pausa (2,2–2,9 s), cambios de volumen, desactivación y reactivación musical.
El usuario debe introducir `s` solo si realmente oyó música y ambos SFX
y pudo distinguir al menos la pausa y la variación de volumen.

El ejecutable comprueba sin intervención: PCM en buffers, progreso, estado de
pause/resume, volumen y mute, 2 SFX aceptados / 2 duplicados, reintentos de
apertura, parada y ausencia de errores de cierre. Código de salida **0**
significa paso de checks técnicos más declaración humana afirmativa.
**NO es prueba de calidad sonora ni ausencia completa de pérdidas de audio.**

## 3. Integración en juego (Bones) — comprobación independiente

Desde la raíz:
```bat
cd BONES
scripts\build_windows_msvc.bat
```
Arrancar `build\Proyecto144_Bones_ModKit.exe` con la salida activa.
Confirmar por oído música ambiental en arranque y pulsar Space para saltar;
comprobar **un** SFX de salto y **un** SFX de aterrizaje por acción.
Mantener Space pulsado para comprobar que no se reproduce un SFX por frame;
repetir varias veces. No contar prueba del harness como prueba de eventos
emitidos desde las físicas reales de Bones.

Con simulación pausada en un menú futuro, Bones deberá bombear con
`ear_audio_pump()` desde hilo principal; el menú es competencia de Skin.
No declarar validado ese caso hasta implementar/ejecutar el host correspondiente.

## 4. Fallo recuperable real (opcional, manual)

Desconectar o deshabilitar todos los dispositivos de salida **antes**
de ejecutar:
```bat
Ear\scripts\test_windows_audio.bat --retry-device
```

La prueba exige arranque sin dispositivo (si otro queda activo abortará).
Al solicitarlo, reconectar el dispositivo y esperar a que Windows lo active;
pulsar ENTER para invocar **una sola vez** `ear_audio_retry_output()`.
Observar diagnóstico antes/después, `open_attempts`, sonido y cierre.
Si sigue sin dispositivo, código 2: fallo recuperable, no aceptación positiva.

Para repetir la prueba de ausencia de hardware (sin comprobar audible) usar
`--allow-no-device`; esta opción se reserva al CI y **no** valida sonido real.

## 5. Resultados a devolver a Soul

| Punto | Resultado | Evidencia |
|---|---|---|
| WinMM inicia con dispositivo activo | PENDIENTE | status open |
| Se oye música procedural DUNGEON | PENDIENTE | confirmación humana |
| Saltar y aterrizar: SFX únicos | PENDIENTE | test de demo y contadores |
| Volúmenes música/SFX y mute | PENDIENTE | status + oído |
| Pause y resume sin reapertura | PENDIENTE | status attempts |
| Pump sin tick de física | PENDIENTE | buffers incrementales |
| Stop: reset/unprepare/close, error 0 | PENDIENTE | status closed |
| No-device/retry real | PENDIENTE | status error/native |
| Aceptación de Soul | PENDIENTE | revisión PR #13 |

Incluir texto completo de consola, captura opcional, errores y decisión
sobre audibilidad. Si algún punto falla, informar en issue #18 y PR #13.
**No fusionar** únicamente por éxito en GitHub Actions.

Códigos: 0 completo, 2 dispositivo/recovery, 3 estado WinMM/buffers/controles/
cleanup, 5 inicio ModKit, 6 escenario de reconexión no preparado, 7 sin
confirmación audible; cualquier compilación fallida tendrá un código no cero.

### Declaración
Resultado de prueba física: APTO / NO APTO / NO EJECUTADO: __________
Firma o confirmación del operador: __________
Revisión de Soul: __________
