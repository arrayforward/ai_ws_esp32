set(OPUS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/opus-1.6.1)

file(GLOB OPUS_SRC       ${OPUS_DIR}/src/*.c)
file(GLOB OPUS_CELT      ${OPUS_DIR}/celt/*.c)
file(GLOB OPUS_SILK      ${OPUS_DIR}/silk/*.c)
file(GLOB OPUS_SILK_FIX  ${OPUS_DIR}/silk/fixed/*.c)

list(REMOVE_ITEM OPUS_SRC
    ${OPUS_DIR}/src/analysis.c
    ${OPUS_DIR}/src/mlp.c
    ${OPUS_DIR}/src/mlp_data.c
    ${OPUS_DIR}/src/opus_projection_encoder.c
    ${OPUS_DIR}/src/opus_projection_decoder.c
    ${OPUS_DIR}/src/mapping_matrix.c
)

idf_component_register(
    SRCS "../../../components/convai_ws/convai_protocol.c"
         "../../../components/convai_ws/convai_codec_g711a.c"
         "../../../components/convai_ws/convai_codec.c"
         "../../../components/convai_ws/codec_pcm.c"
         "../../../components/convai_ws/codec_g711.c"
         "../../../components/convai_ws/codec_ima_adpcm.c"
         "../../../components/convai_ws/codec_opus.c"
         "../../../components/convai_ws/convai_ring.c"
         ${OPUS_SRC} ${OPUS_CELT} ${OPUS_SILK} ${OPUS_SILK_FIX}
    INCLUDE_DIRS "../../../components/convai_ws/include" ${OPUS_DIR}/include
    PRIV_INCLUDE_DIRS ${OPUS_DIR}/src ${OPUS_DIR}/celt ${OPUS_DIR}/silk ${OPUS_DIR}/silk/fixed
    REQUIRES cjson
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    CONFIG_CONVAI_ENABLE_OPUS=1
    OPUS_BUILD
    FIXED_POINT
    DISABLE_FLOAT_API
    CUSTOM_MODES
    VAR_ARRAYS
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
    -Wno-error=maybe-uninitialized
    -Wno-error=unused-function
    -Wno-error=sign-compare
)
