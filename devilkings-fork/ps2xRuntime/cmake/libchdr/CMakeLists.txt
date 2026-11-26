# Shim that builds the vendored libchdr for the Windows launcher only.
#
# libchdr's own CMakeLists.txt is left untouched. Building it through this project instead of add_subdirectory()ing the
# upstream CMakeLists keeps every upstream option (BUILD_SHARED_LIBS, WITH_LZMA_ASM, ...) out of both the parent cache
# and the rest of the runtime build: only this subdirectory enables C, and only these three static libraries are made.
# libchdr v0.3.0, BSD-3-Clause, pinned source in third_party/libchdr (see THIRD_PARTY_NOTICES.txt).

project(ShinobiLibchdr LANGUAGES C)

set(CHDR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/libchdr")

add_library(shinobi-chdr-lzma STATIC "${CHDR_DIR}/deps/lzma-25.01/src/LzmaDec.c")
add_library(shinobi-miniz STATIC "${CHDR_DIR}/deps/miniz-3.1.1/miniz.c")
target_compile_definitions(shinobi-miniz PRIVATE
    MINIZ_NO_ARCHIVE_APIS MINIZ_NO_DEFLATE_APIS MINIZ_NO_STDIO MINIZ_NO_TIME)
add_library(shinobi-zstd STATIC "${CHDR_DIR}/deps/zstd-1.5.7/zstddeclib.c")

add_library(shinobi-chdr STATIC
    "${CHDR_DIR}/src/libchdr_bitstream.c"
    "${CHDR_DIR}/src/libchdr_cdrom.c"
    "${CHDR_DIR}/src/libchdr_chd.c"
    "${CHDR_DIR}/src/libchdr_codec_cdfl.c"
    "${CHDR_DIR}/src/libchdr_codec_cdlz.c"
    "${CHDR_DIR}/src/libchdr_codec_cdzl.c"
    "${CHDR_DIR}/src/libchdr_codec_cdzs.c"
    "${CHDR_DIR}/src/libchdr_codec_flac.c"
    "${CHDR_DIR}/src/libchdr_codec_huff.c"
    "${CHDR_DIR}/src/libchdr_codec_lzma.c"
    "${CHDR_DIR}/src/libchdr_codec_zlib.c"
    "${CHDR_DIR}/src/libchdr_codec_zstd.c"
    "${CHDR_DIR}/src/libchdr_flac.c"
    "${CHDR_DIR}/src/libchdr_huffman.c"
)
target_include_directories(shinobi-chdr PUBLIC "${CHDR_DIR}/include")
target_compile_definitions(shinobi-chdr PRIVATE
    WANT_RAW_DATA_SECTOR=1 WANT_SUBCODE=1 VERIFY_BLOCK_CRC=1)
if(MSVC)
    target_compile_definitions(shinobi-chdr PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
target_link_libraries(shinobi-chdr PRIVATE shinobi-chdr-lzma shinobi-miniz shinobi-zstd)
