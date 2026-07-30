idf_component_register(
    SRCS "convai_ws.c" "convai_protocol.c" "convai_codec_g711a.c"
         "convai_codec.c" "codec_pcm.c" "codec_g711.c"
         "codec_ima_adpcm.c" "codec_opus.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_websocket_client cjson esp_timer opus
)
