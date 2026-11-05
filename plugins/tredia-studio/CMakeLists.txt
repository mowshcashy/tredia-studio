cmake_minimum_required(VERSION 3.28...3.30)

# Tredia Studio's built-in Tredia features: sign-in, Turbo (Tredia relays the stream to every platform),
# streaming to several places from this PC, the overlays, the stream report and uploading recordings.

find_package(Qt6 REQUIRED Widgets Network)

add_library(tredia-studio MODULE)
add_library(OBS::tredia-studio ALIAS tredia-studio)

target_sources(
  tredia-studio
  PRIVATE
    tredia-plugin.cpp
    tredia-api.cpp
    tredia-api.hpp
    tredia-dock.cpp
    tredia-dock.hpp
    multi-output.cpp
    multi-output.hpp
    secure-store.cpp
    secure-store.hpp
)

target_link_libraries(
  tredia-studio
  PRIVATE OBS::frontend-api OBS::libobs Qt::Widgets Qt::Network $<$<PLATFORM_ID:Windows>:crypt32>
)

set_target_properties_obs(tredia-studio PROPERTIES FOLDER plugins PREFIX "" AUTOMOC ON AUTOUIC OFF AUTORCC OFF)

# Qt Network needs a TLS backend for HTTPS (tredia.cloud). OBS doesn't deploy Qt's tls plugins, so ship
# Windows' own (Schannel) backend next to the executable.
if(OS_WINDOWS)
  get_target_property(_tredia_qtcore Qt6::Core IMPORTED_LOCATION_RELEASE)
  if(NOT _tredia_qtcore)
    get_target_property(_tredia_qtcore Qt6::Core IMPORTED_LOCATION)
  endif()
  cmake_path(GET _tredia_qtcore PARENT_PATH _tredia_qtbin)
  cmake_path(GET _tredia_qtbin PARENT_PATH _tredia_qtroot)
  set(_tredia_tls "${_tredia_qtroot}/plugins/tls/qschannelbackend.dll")
  if(EXISTS "${_tredia_tls}")
    install(FILES "${_tredia_tls}" DESTINATION "${OBS_EXECUTABLE_DESTINATION}/tls" COMPONENT Runtime)
  else()
    message(FATAL_ERROR "Tredia Studio needs Qt's Schannel TLS backend, not found at ${_tredia_tls}")
  endif()
endif()
