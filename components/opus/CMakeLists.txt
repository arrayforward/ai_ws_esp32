# ESP-IDF component for the vendored Opus 1.6.1 sources.
# Fixed-point (integer) build: FIXED_POINT + DISABLE_FLOAT_API,
# same configuration as the goldieos WS63 build.

set(OPUS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/opus-1.6.1)

file(GLOB OPUS_SRC       ${OPUS_DIR}/src/*.c)
file(GLOB OPUS_CELT      ${OPUS_DIR}/celt/*.c)
file(GLOB OPUS_SILK      ${OPUS_DIR}/silk/*.c)
file(GLOB OPUS_SILK_FIX  ${OPUS_DIR}/silk/fixed/*.c)

# float-only sources (analysis/MLP) and ambisonics projection are excluded
# in a fixed-point, mono/stereo build
list(REMOVE_ITEM OPUS_SRC
    ${OPUS_DIR}/src/analysis.c
    ${OPUS_DIR}/src/mlp.c
    ${OPUS_DIR}/src/mlp_data.c
    ${OPUS_DIR}/src/opus_projection_encoder.c
    ${OPUS_DIR}/src/opus_projection_decoder.c
    ${OPUS_DIR}/src/mapping_matrix.c
)

idf_component_register(
    SRCS ${OPUS_SRC} ${OPUS_CELT} ${OPUS_SILK} ${OPUS_SILK_FIX}
    INCLUDE_DIRS ${OPUS_DIR}/include
    PRIV_INCLUDE_DIRS ${OPUS_DIR}/src ${OPUS_DIR}/celt ${OPUS_DIR}/silk ${OPUS_DIR}/silk/fixed
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    OPUS_BUILD
    FIXED_POINT
    DISABLE_FLOAT_API
    CUSTOM_MODES
    VAR_ARRAYS
    HAVE_LRINTF
    HAVE_LRINT
)
