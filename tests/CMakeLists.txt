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

add_executable(test_metadata_blob
    test_metadata_blob.cpp
    ../src/core/MetadataBlob.cpp
)
target_include_directories(test_metadata_blob PRIVATE ../src)
target_link_libraries(test_metadata_blob PRIVATE Qt6::Core Qt6::Test PkgConfig::ZSTD)
target_compile_features(test_metadata_blob PRIVATE cxx_std_26)
add_test(NAME test_metadata_blob COMMAND test_metadata_blob)

add_executable(test_schema
    test_schema.cpp
    ../src/core/MetadataBlob.cpp
    ../src/core/Rating.cpp
    ../src/db/Database.cpp
    ../src/fs/PathResolver.cpp
)
target_include_directories(test_schema PRIVATE ../src)
target_link_libraries(test_schema PRIVATE Qt6::Core Qt6::Sql Qt6::Test PkgConfig::ZSTD)
target_compile_features(test_schema PRIVATE cxx_std_26)
add_test(NAME test_schema COMMAND test_schema)

add_executable(test_tag_rating
    test_tag_rating.cpp
    ../src/core/Rating.cpp
    ../src/scanner/TagRating.cpp
)
target_include_directories(test_tag_rating PRIVATE ../src)
target_link_libraries(test_tag_rating PRIVATE Qt6::Core Qt6::Test PkgConfig::TAGLIB)
target_compile_features(test_tag_rating PRIVATE cxx_std_26)
add_test(NAME test_tag_rating COMMAND test_tag_rating)

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

add_executable(test_lastfm_api
    test_lastfm_api.cpp
    ../src/scrobble/LastFmApi.cpp
)
target_include_directories(test_lastfm_api PRIVATE ../src)
target_link_libraries(test_lastfm_api PRIVATE Qt6::Core Qt6::Network Qt6::Test)
target_compile_features(test_lastfm_api PRIVATE cxx_std_26)
add_test(NAME test_lastfm_api COMMAND test_lastfm_api)

add_executable(test_artwork_resolver
    test_artwork_resolver.cpp
    ../src/scanner/ArtworkResolver.cpp
)
target_include_directories(test_artwork_resolver PRIVATE ../src)
target_link_libraries(test_artwork_resolver PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
target_compile_features(test_artwork_resolver PRIVATE cxx_std_26)
add_test(NAME test_artwork_resolver COMMAND test_artwork_resolver)
set_tests_properties(test_artwork_resolver PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Search engine tests (no Qt Widgets dependency)
add_executable(test_search_query
    test_search_query.cpp
    ../src/search/SearchQuery.cpp
)
target_include_directories(test_search_query PRIVATE ../src)
target_link_libraries(test_search_query PRIVATE Qt6::Core Qt6::Test)
target_compile_features(test_search_query PRIVATE cxx_std_26)
add_test(NAME test_search_query COMMAND test_search_query)

add_executable(test_fuzzy_match
    test_fuzzy_match.cpp
    ../src/search/FuzzyMatch.cpp
)
target_include_directories(test_fuzzy_match PRIVATE ../src)
target_link_libraries(test_fuzzy_match PRIVATE Qt6::Core Qt6::Test)
target_compile_features(test_fuzzy_match PRIVATE cxx_std_26)
add_test(NAME test_fuzzy_match COMMAND test_fuzzy_match)

add_executable(test_search_index
    test_search_index.cpp
    ../src/search/FuzzyMatch.cpp
    ../src/search/SearchIndex.cpp
    ../src/search/SearchQuery.cpp
)
target_include_directories(test_search_index PRIVATE ../src)
target_link_libraries(test_search_index PRIVATE Qt6::Core Qt6::Test)
target_compile_features(test_search_index PRIVATE cxx_std_26)
add_test(NAME test_search_index COMMAND test_search_index)
