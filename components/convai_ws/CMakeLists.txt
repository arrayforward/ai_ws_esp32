idf_component_register(
    SRCS "convai_ws.c" "convai_protocol.c" "convai_codec_g711a.c"
         "convai_codec.c" "codec_pcm.c" "codec_g711.c"
         "codec_ima_adpcm.c" "codec_opus.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_websocket_client cjson esp_timer esp-tls opus
)

# Optional custom CA for wss:// with self-signed certificates:
# drop a PEM file at components/convai_ws/certs/server_ca.pem and it
# gets embedded into the firmware (CONVAI_WSS_CUSTOM_CA is defined).
set(CUSTOM_CA ${CMAKE_CURRENT_SOURCE_DIR}/certs/server_ca.pem)
if(EXISTS ${CUSTOM_CA})
    target_add_binary_data(${COMPONENT_LIB} ${CUSTOM_CA} TEXT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONVAI_WSS_CUSTOM_CA=1)
    message(STATUS "convai_ws: embedding custom WSS CA ${CUSTOM_CA}")
endif()
