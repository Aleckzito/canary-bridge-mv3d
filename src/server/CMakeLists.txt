target_sources(
    ${CORE_TARGET_NAME}
    PRIVATE network/connection/connection.cpp
            admin/python_admin_bridge.cpp
            admin/mv3d_editor_bridge.cpp
            network/message/networkmessage.cpp
            network/message/outputmessage.cpp
            network/protocol/protocol.cpp
            network/protocol/protocolgame.cpp
            network/protocol/protocollogin.cpp
            network/protocol/protocolstatus.cpp
            network/webhook/webhook.cpp
            server.cpp
            signals.cpp
)

