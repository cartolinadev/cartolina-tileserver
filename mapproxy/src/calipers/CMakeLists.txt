# program
define_module(BINARY mp-calipers
  DEPENDS vts-libs service gdal-drivers geometry geo mapproxy-core
  Boost_FILESYSTEM
  Boost_PROGRAM_OPTIONS)

set(mp-calipers_SOURCES
  calipers.hpp calipers.cpp
  )

# mp-calipers is a library: mapproxy-tiling and mapproxy-setup-resource
# link it to measure a dataset against a reference frame.
add_library(mp-calipers ${mp-calipers_SOURCES})
target_link_libraries(mp-calipers ${MODULE_LIBRARIES})
target_compile_definitions(mp-calipers PRIVATE ${MODULE_DEFINITIONS})
buildsys_library(mp-calipers)
