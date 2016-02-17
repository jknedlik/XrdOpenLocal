include( XRootDCommon )

#-------------------------------------------------------------------------------
# Modules
#-------------------------------------------------------------------------------
set( LIB_XRD_FILECACHE  XrdFileCache-${PLUGIN_VERSION} )

#-------------------------------------------------------------------------------
# Shared library version
#-------------------------------------------------------------------------------

#-------------------------------------------------------------------------------
# The XrdFileCache library
#-------------------------------------------------------------------------------
add_library(
  ${LIB_XRD_FILECACHE}
  MODULE
  XrdFileCache/XrdFileCache.cc              XrdFileCache/XrdFileCache.hh
  XrdFileCache/XrdFileCacheFactory.cc       XrdFileCache/XrdFileCacheFactory.hh
  XrdFileCache/XrdFileCachePrefetch.cc      XrdFileCache/XrdFileCachePrefetch.hh
  XrdFileCache/XrdFileCacheStats.hh
  XrdFileCache/XrdFileCacheInfo.cc          XrdFileCache/XrdFileCacheInfo.hh
  XrdFileCache/XrdFileCacheIOEntireFile.cc  XrdFileCache/XrdFileCacheIOEntireFile.hh
  XrdFileCache/XrdFileCacheIOFileBlock.cc   XrdFileCache/XrdFileCacheIOFileBlock.hh
  XrdFileCache/XrdFileCacheDecision.hh)

target_link_libraries(
  ${LIB_XRD_FILECACHE}
  XrdPosix
  XrdCl
  XrdUtils
  XrdServer
  pthread )

set_target_properties(
  ${LIB_XRD_FILECACHE}
  PROPERTIES
  INTERFACE_LINK_LIBRARIES ""
  LINK_INTERFACE_LIBRARIES "" )

#-------------------------------------------------------------------------------
# xrdpfc_print
#-------------------------------------------------------------------------------
add_executable(
  xrdpfc_print
  XrdFileCache/XrdFileCachePrint.hh  XrdFileCache/XrdFileCachePrint.cc
  XrdFileCache/XrdFileCacheInfo.hh  XrdFileCache/XrdFileCacheInfo.cc)

target_link_libraries(
  xrdpfc_print
  XrdServer
  XrdCl
  XrdUtils )

#-------------------------------------------------------------------------------
# Install
#-------------------------------------------------------------------------------
install(
  TARGETS ${LIB_XRD_FILECACHE}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} )

install(
  TARGETS xrdpfc_print
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} )


install(
  FILES
  ${PROJECT_SOURCE_DIR}/docs/man/xrdpfc_print.8
  DESTINATION ${CMAKE_INSTALL_MANDIR}/man8 )

