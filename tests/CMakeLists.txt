function(add_muzaiten_test name source)
    add_executable(${name} ${source}
        ../src/core/Rating.cpp
        ../src/ui/StarRating.cpp
    )
    target_include_directories(${name} PRIVATE
        ../src
        ${CMAKE_CURRENT_BINARY_DIR}/../generated
    )
    target_link_libraries(${name} PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    target_compile_features(${name} PRIVATE cxx_std_26)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_muzaiten_test(test_rating test_rating.cpp)
set_tests_properties(test_rating PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_schema
    test_schema.cpp
    ../src/core/Rating.cpp
    ../src/db/Database.cpp
    ../src/fs/PathResolver.cpp
)
target_include_directories(test_schema PRIVATE ../src)
target_link_libraries(test_schema PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
target_compile_features(test_schema PRIVATE cxx_std_26)
add_test(NAME test_schema COMMAND test_schema)

add_executable(test_path_resolver
    test_path_resolver.cpp
    ../src/fs/PathResolver.cpp
)
target_include_directories(test_path_resolver PRIVATE ../src)
target_link_libraries(test_path_resolver PRIVATE Qt6::Core Qt6::Test)
target_compile_features(test_path_resolver PRIVATE cxx_std_26)
add_test(NAME test_path_resolver COMMAND test_path_resolver)

add_executable(test_mpd_config
    test_mpd_config.cpp
    ../src/mpd/MpdConfig.cpp
)
target_include_directories(test_mpd_config PRIVATE ../src)
target_link_libraries(test_mpd_config PRIVATE Qt6::Core Qt6::Test)
target_compile_features(test_mpd_config PRIVATE cxx_std_26)
add_test(NAME test_mpd_config COMMAND test_mpd_config)

add_executable(test_mpd_client
    test_mpd_client.cpp
    ../src/mpd/MpdClient.cpp
)
target_include_directories(test_mpd_client PRIVATE ../src)
target_link_libraries(test_mpd_client PRIVATE Qt6::Core Qt6::Network Qt6::Test)
target_compile_features(test_mpd_client PRIVATE cxx_std_26)
add_test(NAME test_mpd_client COMMAND test_mpd_client)

add_executable(test_artwork_resolver
    test_artwork_resolver.cpp
    ../src/scanner/ArtworkResolver.cpp
)
target_include_directories(test_artwork_resolver PRIVATE ../src)
target_link_libraries(test_artwork_resolver PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
target_compile_features(test_artwork_resolver PRIVATE cxx_std_26)
add_test(NAME test_artwork_resolver COMMAND test_artwork_resolver)
set_tests_properties(test_artwork_resolver PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
