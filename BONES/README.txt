PROYECTO 1.44 — BONES 0.4.0 — MODKIT EN C11
===========================================

BONES es el programador responsable de la BASE del juego; este repositorio
contiene el MOTOR central, no la totalidad de las especialidades.

NOVEDAD: ModKit ABI 1 (propuesta para Soul). Permite dos rutas:
  - módulos fuente insertables: modules/<autor>/modkit.c, sin modificar core;
    generador src/generated/modkit_registry.c en compilación.
  - DLL/.so opcionales durante desarrollo, mismo descriptor + callbacks.
El EXE final de Windows utiliza enlace ESTÁTICO sin DLLs externas.

EJECUCIÓN Linux:
  make                  -> ejecutable de captura con módulos actuales
  make modkit_demo      -> corre la simulación con 5 módulos
  make modkit_test      -> pruebas módulos y carga .so de ejemplo
  make test             -> cuatro suites de regresión preexistentes
  make modkit_preview   -> build/modkit_preview.ppm, imagen REAL de Eye

WINDOWS (x64, compilación pendiente de verificar en un Windows real):
  scripts\build_windows_msvc.bat
  scripts\build_windows_mingw.bat
  scripts\build_windows_modkit_dev.bat (DLL y host, SOLO desarrollo)

CONTRATOS PRINCIPALES:
  include/bones/modkit.h       ABI v1 para especialistas (no incluye BonesEngine)
  include/bones/modkit_host.h  API host fuente, usada por aplicación central
  include/bones/bones.h        antigua API2 conservada
  include/bones/foundation.h   API de entidades/estado BSF1 conservada
  docs/MODKIT_V0_4_0_CONTRATO.txt
  docs/MODKIT_V0_4_0_GUIA_ESPECIALISTAS.txt
  docs/MODKIT_V0_4_0_ESTADO_Y_CAMBIOS.txt
  docs/MODKIT_V0_4_0_REVISION_SOUL.txt

LÍMITES:
  La DLL NO es sandbox; solo ejecutar módulos de desarrolladores confiables.
  No hay hot reload ni DLL requeridas para jugar. Los proveedores Ear, Brain,
  Skin y Mouth son conectores sin especialidades desarrolladas aún.
  Eye reutiliza renderizador original con 3 materiales.
  No hay certificación de ejecutable Windows <=1.474.560 bytes.
  El ZIP no es el juego completo: es una base probada y extensible.
