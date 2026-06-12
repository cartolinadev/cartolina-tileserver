# tools
define_module(BINARY tools
  DEPENDS mapproxy-core service

  Boost_FILESYSTEM
  Boost_PROGRAM_OPTIONS
  )

# ----------------------------------------------------------------------
# vts::TileIndex to mapproxy's mapped::TileIndex
set(mapproxy-ti2mmti_SOURCES
  ti2mmti.cpp
  )

add_executable(mapproxy-ti2mmti ${mapproxy-ti2mmti_SOURCES})
target_link_libraries(mapproxy-ti2mmti ${MODULE_LIBRARIES})
target_compile_definitions(mapproxy-ti2mmti PRIVATE ${MODULE_DEFINITIONS})
buildsys_binary(mapproxy-ti2mmti)
set_target_version(mapproxy-ti2mmti ${vts-mapproxy_VERSION})

# ----------------------------------------------------------------------
# metanode store inspection and self-test tool
set(mapproxy-mnstore_SOURCES
  mnstore.cpp
  )

add_executable(mapproxy-mnstore ${mapproxy-mnstore_SOURCES})
target_link_libraries(mapproxy-mnstore ${MODULE_LIBRARIES})
target_compile_definitions(mapproxy-mnstore PRIVATE ${MODULE_DEFINITIONS})
buildsys_binary(mapproxy-mnstore)
set_target_version(mapproxy-mnstore ${vts-mapproxy_VERSION})

# ----------------------------------------------------------------------
# tile index diff (RFC 7 parity gate helper)
set(mapproxy-tidiff_SOURCES
  tidiff.cpp
  )

add_executable(mapproxy-tidiff ${mapproxy-tidiff_SOURCES})
target_link_libraries(mapproxy-tidiff ${MODULE_LIBRARIES})
target_compile_definitions(mapproxy-tidiff PRIVATE ${MODULE_DEFINITIONS})
buildsys_binary(mapproxy-tidiff)
set_target_version(mapproxy-tidiff ${vts-mapproxy_VERSION})

# ----------------------------------------------------------------------
# RFC 7 texelSize calibration spike
set(mapproxy-texel-spike_SOURCES
  texelspike.cpp
  )

add_executable(mapproxy-texel-spike ${mapproxy-texel-spike_SOURCES})
target_link_libraries(mapproxy-texel-spike ${MODULE_LIBRARIES})
target_compile_definitions(mapproxy-texel-spike PRIVATE ${MODULE_DEFINITIONS})
buildsys_binary(mapproxy-texel-spike)
set_target_version(mapproxy-texel-spike ${vts-mapproxy_VERSION})

# ----------------------------------------------------------------------
# vts::TileIndex and mapped::TileIndex query tool
set(mapproxy-querymmti_SOURCES
  querymmti.cpp
  )

add_executable(mapproxy-querymmti ${mapproxy-querymmti_SOURCES})
target_link_libraries(mapproxy-querymmti ${MODULE_LIBRARIES})
target_compile_definitions(mapproxy-querymmti PRIVATE ${MODULE_DEFINITIONS})
buildsys_binary(mapproxy-querymmti)
set_target_version(mapproxy-querymmti ${vts-mapproxy_VERSION})

# ----------------------------------------------------------------------
# --- installation
# ----------------------------------------------------------------------

# RFC 7 operator/diagnostic tools
install(TARGETS mapproxy-mnstore mapproxy-tidiff mapproxy-texel-spike
  RUNTIME DESTINATION bin COMPONENT tools)
