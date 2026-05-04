CANARY PYTHON ADMIN BRIDGE - MVP

Archivos:
- python_admin_bridge.hpp
- python_admin_bridge.cpp
- python_control_gui.py

Objetivo:
Exponer un control plane local minimo para operar Canary desde un GUI Python.

Transporte actual:
- HTTP local
- bind solo en 127.0.0.1
- puerto por defecto: 18991

Variables de entorno:
- CANARY_PYTHON_BRIDGE_DISABLED=1
  Desactiva el bridge al arrancar el servidor

- CANARY_PYTHON_BRIDGE_PORT=18991
  Cambia el puerto local del bridge

Endpoints MVP:
- GET  /status
- POST /command/save
- POST /command/reload-all
- POST /command/open
- POST /command/close
- POST /command/shutdown

GUI Python:
Ejecutar:
python python_control_gui.py

Ubicacion:
src/server/admin/python_control_gui.py

Limites de esta primera version:
- no replica la consola del server
- no lista players individualmente
- no expone talk actions ni mensajes aun
- no expone containers, stats ni minimap

Siguiente paso natural:
- agregar players online detallados
- broadcast message
- kick / ban / mute
- logs live
- recargas por subsistema
