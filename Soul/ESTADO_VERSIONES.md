# Estado de versiones — Proyecto 1.44

**Revisión:** 21/10/2026 · rama `main` · árbol observado `8a3216adb0e6239130c6b88c47c70e0512b60659`.

| Especialista | Fuente/versión visible en GitHub | Coincidencia con entregas localizadas | Integración con el juego |
|---|---|---|---|
| BONES | `BONES/README.md`: **0.4.0 ModKit** y fuentes de núcleo, tests, scripts | 0.4.0 aparece en código y documentos; el archivo `ESTADO_SINCRONIZACION_0_4.md` afirma todavía incompleto mientras README afirma sincronización. No se contrastaron SHA contra ZIP | **Demo Bones+Eye legacy; otros proveedores ModKit son conectores** |
| EYE | `Eye/` **0.5.1**, generadores EYE1–EYE5, fuentes y tests | `Eye/ESTADO_SUBIDA.md` declara 102/102 y hashes Git blob coincidentes con el ZIP; **declaración del responsable, no revalidación independiente** | No integrado con Bones ModKit/escena ampliada; las pruebas usan dobles en casos de Bones |
| BRAIN | `Brain/README.md` **0.2.0**, código de IA y tests | Versión nominal coincide con la entrega v0.2 localizable | Demo y stub; no integrado con Bones real |
| EAR | `Ear/README.md` **0.2.0**, motor y tests | Versión nominal coincide con la entrega v0.2 localizable | Prototipo aislado; puente Bones por verificar |
| SKIN | `Skin/v0_2/`, UI **0.2.0** | Versión nominal coincide con entrega v0.2 localizable | Interfaz autónoma; no integrada con Bones/Eye reales |
| MOUTH | `Mouth/README.md`: solo guía y script de importación | Entrega `MOUTH_P2P_Bones_Visual_v0_2.zip` localizada en archivos del proyecto | **Código v0.1/v0.2 aún no publicado; no hay integración P2P** |

## Hallazgos verificables del código

- El árbol GitHub carece de compilación principal en la raíz; `BONES/scripts/build_windows_msvc.bat` genera `BONES/build/Proyecto144_Bones_ModKit.exe`, prototipo de Bones con el renderizador Eye heredado y módulos restantes de muestra.
- `BONES/modules/brain/modkit.c` se declara expresamente un placeholder sin lógica de Brain.
- `BONES/modules/eye/modkit.c` convierte a escena de 3 materiales y omite IDs >2; por tanto, **no reproduce toda la gama de Eye 0.5.1**.
- `BONES/docs/MODKIT_V0_4_0_ESTADO_Y_CAMBIOS.txt` reconoce ausencia de módulos reales especializados, mundo/guardado/networking completo y validación Windows.
- No se ha ejecutado compilación ni prueba sobre este checkout en esta revisión. No confundir el código publicado con juego integrado o aceptado.

## Próximas puertas
1. Validar fuente de Bones 0.4 mediante manifest/hashes y compilar el EXE Windows de demo.
2. Integrar adaptadores reales EYE, BRAIN, EAR, SKIN y MOUTH sin romper `BONES_API_VERSION=2` ni fijar ABI experimental sin aprobación.
3. Ejecutar pruebas de integración, sanitizadores, LAN y compilación PE final con medida de 1.474.560 bytes.

Registro detallado: [DISCREPANCIAS.md](DISCREPANCIAS.md).
