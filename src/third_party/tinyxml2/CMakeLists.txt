add_library(tinyxml2 STATIC tinyxml2.cpp)
target_include_directories(tinyxml2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

#set_property(TARGET openMVG_tinyxml2 PROPERTY FOLDER OpenMVG/3rdParty)

#install(TARGETS openMVG_tinyxml2 DESTINATION lib EXPORT openMVG-targets)
