add_library(TinyEXIF STATIC TinyEXIF.cpp)

target_include_directories(TinyEXIF PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(TinyEXIF tinyxml2)

#set_property(TARGET openMVG_TinyEXIF PROPERTY FOLDER OpenMVG/3rdParty)

#install(TARGETS openMVG_TinyEXIF DESTINATION lib EXPORT openMVG-targets)
