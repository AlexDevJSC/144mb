# Informe parcial para Soul — Mouth LAN 0.4.0 y dependencias de integración
Fecha: 21/10/2026. **NO es aceptación final de multijugador gráfico.**

## Referencia exacta de código y documentos
Repositorio: https://github.com/AlexDevJSC/144mb
API vigente: Mouth/docs/CONTRATO_SESIONES_BONES_SKIN_v1.md
Versiones: Mouth 0.4.0; MOUTH_SESSION_API_VERSION=1; M144/1; Bones ModKit ABI 1.
Módulo fuente instalado: BONES/modules/mouth; entrada única bones_mod_mouth_query.
No se han modificado los archivos C del protocolo ni la API tras el contrato v0.4.0.
Commit de pruebas de pérdida: 29b14654879ec2f8a21e2b624dcea97506162a47.
Registro documental inicial del test: d99379d749f2338cf606ee1390ebd311f4a81349.

## CI verificado (solo transporte y Bones aislado, no UI)
- Run main 39614303198: Windows MSVC y Linux GCC: SUCCESS.
- Run main 39614541148: Windows MSVC y Linux GCC: SUCCESS.
- Compilación C y pruebas API/codec/ModKit.
- Test loopback 2 procesos: host ejecuta Bones Foundation y submit_move.
- Comprueba que sólo el actor remoto owner=2 cambia al recibir comandos
  del cliente, el actor local permanece independiente.
- Test de IP inválida, sin anfitrión, world mismatch, wire mismatch,
  paquete de control fuera de sesión, salida LEAVE y timeout.
- Test NUEVO UDP relay test_lan_loss_proxy.py: 12 s de cliente,
  pérdida aleatoria 15% y un corte continuo de 1,2 s.
  Windows: 229 descartes, 96 durante corte; final tick cliente 732.
  Linux:   229 descartes, 96 durante corte; final tick cliente 738.
  En ambas plataformas se recuperaron snapshots confirmados tras el corte.
- Son ensayos sobre localhost, no demuestran latencia/jitter WAN ni LAN
  física ni 30 minutos continuos.
- La medida previa del ejecutable de pruebas BONES en un run MSVC fue
  219.648 bytes; no implica el tamaño del juego combinado con futuras
  ramas Eye/Ear/Brain/Skin ni valida el objetivo de 1,44 MB definitivo.

## Estado del flujo solicitado
  Individual: descriptor Mouth inerte sin socket = disponible por ABI.
  Crear LAN: demo sí; BONES/src/app/win32_main.c NO ofrece esa selección.
  Join por IPv4: demo sí cuando el mundo ya es conocido.
  Cliente mueve actor2 y host confirma: demo sí (physics Bones).
  Representar ambos actores en ventana gráfica cliente: NO implementado.
  Skin UI seleccionar/unirse/desconectar: issue #19, NO integrado en main.
  Bones host integrado en arranque Win32: issue #20, NO integrado en main.
  Cerrar limpio: test demo PASS, integración UI gráfica pendiente.
  Dos ordenadores Windows físicos: NO realizado.
  Sesión >=30 min y pérdida sostenida en LAN física: NO realizado.
  Descubrir mundo desconocido solo con IP:puerto: NO implementado.
  Autenticación criptográfica / Internet / NAT: NO implementados.

## Riesgos concretos para revisión de Soul
1. Orden de inicialización: spawn remoto owner=2 y configurar Mouth
   ANTES de registrar builtins; no duplicar bones_mod_mouth_query.
2. Autoridad: comando cliente no define actor ni issuer; host usa
   IDs creados/verificados por Bones, sin motor físico cliente.
3. Tiempo de vida: MouthLan sobrevive a BonesModHost; destroy antes
   de liberar contexto, no abrir sockets en singleplayer.
4. Identidad de mundo: join actual exige world_id, seed y generator_version
   iguales antes del HELLO. Diseño de ampliación no aprobado en issue #23.
5. Save/checkpoint y compatibilidad visual Eye aún no negociados;
   no admitir partida guardada divergente sin autorización.
6. Rama experimental PR #21 (BONES 0.4.2) tiene CI loopback fallida
   en run 39608819207 (cliente phase=8 cause=9). Ese SHA NO está
   integrado en main y no justifica cambio de Mouth sin reproducción
   y análisis de orden de apagado/timing en esa rama.

## Evidencias y próximos criterios
- QA reproducible: Mouth/docs/PLAN_PRUEBAS_LAN_CONJUNTAS_v1.md.
- Diseño M144/2 solo propuesto:
  Mouth/docs/PROPUESTA_DESCUBRIMIENTO_MUNDOS_LAN_v0.md.
- Aprobación diseño Bones/Soul: issue #23.
- Integración Skin: issue #19; Bones: issue #20.
- Antes de aprobación final: test conjunto con un SHA que incluya
  el menú Skin y host Bones; pruebas de 2 PCs Windows en LAN real,
  video/capturas del cliente gráfico Eye, reconexión, medida PE
  integrado tras todas las fusiones y revisión de Soul.

Conclusión: **Mouth 0.4.0 apto como API/transport para seguir
integrando**; no se ha completado aún el gameplay LAN gráfico.
No promover como Internet P2P seguro ni como implementación pública.
