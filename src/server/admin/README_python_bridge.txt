CANARY PYTHON ADMIN BRIDGE - MVP
Mantenido por: Codex / Alex / Claude (Anthropic)

Archivos:
- python_admin_bridge.hpp
- python_admin_bridge.cpp
- python_control_gui.py

Objetivo:
Exponer un control plane local minimo para operar Canary desde un GUI Python.
Complementa el MV3D Editor Bridge (TCP 7173) permitiendo control administrativo
del servidor sin necesidad de cliente 3D.

Transporte:
- HTTP local (httplib embebido)
- Bind solo en 127.0.0.1 — no expuesto a red externa
- Puerto por defecto: 18991

Variables de entorno:
- CANARY_PYTHON_BRIDGE_DISABLED=1
  Desactiva el bridge al arrancar el servidor

- CANARY_PYTHON_BRIDGE_PORT=18991
  Cambia el puerto local del bridge

Endpoints MVP:
- GET  /status           — estado general del servidor (uptime, players online, modo)
- POST /command/save     — guarda el mapa (equivalente a 0x96 por TCP)
- POST /command/reload-all  — recarga scripts Lua y configuracion
- POST /command/open     — abre el servidor a conexiones de jugadores
- POST /command/close    — cierra el servidor a nuevos ingresos
- POST /command/shutdown — apaga el servidor de forma ordenada

GUI Python:
Ejecutar desde la raiz del proyecto:
  python src/server/admin/python_control_gui.py

Requiere: Python 3.8+, tkinter (incluido en la mayoria de distribuciones)

Limites de esta primera version:
- No replica la consola del servidor en tiempo real
- No lista players individualmente
- No expone talk actions ni mensajes directos
- No expone containers, stats ni minimap
- No tiene autenticacion (solo loopback — no exponer al exterior)

Relacion con MV3D Bridge:
El Python Bridge y el MV3D Editor Bridge (TCP 7173) son independientes.
Ambos pueden estar activos simultaneamente. El Python Bridge es de control
administrativo; el MV3D Bridge es el canal de datos de renderizado y edicion.

Proximos pasos naturales:
- GET /players          — lista de jugadores conectados con posicion y stats
- POST /broadcast       — mensaje a todos los jugadores
- POST /player/kick     — kick por nombre o ID
- POST /player/ban      — ban temporal o permanente
- GET /logs/tail        — ultimas N lineas del log en vivo (SSE o polling)
- POST /reload/scripts  — recarga solo los scripts Lua sin reiniciar
- POST /reload/config   — recarga config.lua en caliente
- Autenticacion basica por token para uso en red local segura
