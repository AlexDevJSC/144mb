# Soul — Secuencia de corrección del primer playtest Windows
**21/10/2026.** Fuente: `Soul/PLAYTEST_BUGS_2026-10-21.md`, protocolo de responsabilidades v1.0 y Bones 0.4.0 ModKit ABI1 experimental.

## Regla de publicación y traspaso
Cada especialista publica **código funcional de su propia competencia**, pruebas, versión/changelog y un `docs/HANDOFF_*.md` propio en rama/PR de GitHub. Incluye commit SHA, qué comprobó realmente, qué necesita el siguiente agente y límites conocidos. Evitar fusiones automáticas y ediciones silenciosas de otros módulos; el director autoriza integración y Soul comprueba regresiones. Una conversación no puede transmitir por sí sola código a otra conversación: el traspaso se hace con la URL del PR/commit, los archivos compartidos y el aviso al director.

## Flujos acotados y paralelizables

**A — Controles y física (prioridad 1).** BONES instrumenta y corrige entrada de ratón, WASD/cámara y salto, registra colliders para la casa y tests; publica -> EYE verifica presentación de orientación/proyección -> BRAIN revalida NPC/colisiones tras merge -> SOUL acepta o reabre. Los pantallazos negros se investigan en paralelo: EYE prueba si el framebuffer tiene flashes -> BONES corrige la presentación Win32/GDI si el defecto procede de ella -> SOUL revalida.

**B — Menús/HUD/E (prioridad 2).** SKIN define por escrito qué necesita de EYE (soporte gráfico para fondo de menú, overlay/composición 2D y paleta, sin apropiación de lógica/UI). EYE publica un **modo de presentación compatible** con un compositor de framebuffer de propiedad del anfitrión -> SKIN construye menús/HUD/prompt de E y entrega interfaz + tests -> BONES conecta entradas, pausa y presentación en `win32_main.c`, además de exponer la selección física de objetivos interactuables -> SOUL y SKIN prueban el EXE. No duplicar renderizadores; `Skin2View` no debe inventar HP/PM/oro aún no definidos.

**C — Música (paralelo).** EAR depura salida de WinMM, test de música y SFX, añade control de volumen/mute mediante interfaz mínima -> publica -> SKIN crea opciones Música/volumen -> BONES enlaza en anfitrión -> SOUL prueba.

**D — Multijugador LAN experimental (paralelo, luego menús).** MOUTH finaliza contrato y pruebas host/unirse y publica versión para `BONES/modules/mouth` -> SKIN crea pantalla de selección solo para modos realmente soportados -> BONES habilita sesión, actor remoto y entrada/salida en host Win32 -> MOUTH realiza prueba de dos procesos y, si es posible, dos ordenadores LAN -> SOUL valida. No etiquetar como P2P Internet completo; no exponer UDP a la red pública.

**E — Acceso directo.** Responsable del instalador corrige creación idempotente del acceso directo a `C:\144\BONES\build\Proyecto144_Bones_ModKit.exe`, sin tocar código de núcleo ni copiar el EXE fuera de la carpeta.

## Criterios de publicación
- Compilar el target declarado (Windows MSVC preferentemente) y correr pruebas afectadas; documentar no probadas de forma honesta.
- Generar registro ModKit y evitar añadir por duplicado los .c incluidos por `modkit.c` (unity build).
- Reportar tamaño **PE Windows del EXE completo** contra 1.474.560 bytes; nunca sustituir por ELF Linux, ZIP o medida de módulo.
- Cambiar ABI o distribución de competencias solo tras aprobación del director y análisis Soul.
- El siguiente especialista recibe el enlace del código **ya publicado y probado**, no una promesa de implementación.
