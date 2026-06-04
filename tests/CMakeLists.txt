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
    ../src/search/Exclusion.cpp
    ../src/search/FuzzyMatch.cpp
    ../src/search/SearchIndex.cpp
    ../src/search/SearchMatcher.cpp
    ../src/search/SearchQuery.cpp
)
target_include_directories(test_search_index PRIVATE ../src)
target_link_libraries(test_search_index PRIVATE Qt6::Core Qt6::Test)
target_compile_features(test_search_index PRIVATE cxx_std_26)
add_test(NAME test_search_index COMMAND test_search_index)

add_executable(test_search_matcher
    test_search_matcher.cpp
    ../src/search/FuzzyMatch.cpp
    ../src/search/SearchMatcher.cpp
    ../src/search/SearchQuery.cpp
)
target_include_directories(test_search_matcher PRIVATE ../src)
target_link_libraries(test_search_matcher PRIVATE Qt6::Core Qt6::Test)
target_compile_features(test_search_matcher PRIVATE cxx_std_26)
add_test(NAME test_search_matcher COMMAND test_search_matcher)

add_executable(test_rank_config
    test_rank_config.cpp
    ../src/search/Exclusion.cpp
    ../src/search/RankConfig.cpp
    ../src/core/MusicSort.cpp
)
target_include_directories(test_rank_config PRIVATE ../src)
target_link_libraries(test_rank_config PRIVATE Qt6::Core Qt6::Test)
target_compile_features(test_rank_config PRIVATE cxx_std_26)
add_test(NAME test_rank_config COMMAND test_rank_config)

add_executable(test_exclusion
    test_exclusion.cpp
    ../src/search/Exclusion.cpp
    ../src/search/FuzzyMatch.cpp
    ../src/search/SearchIndex.cpp
    ../src/search/SearchMatcher.cpp
    ../src/search/SearchQuery.cpp
)
target_include_directories(test_exclusion PRIVATE ../src)
target_link_libraries(test_exclusion PRIVATE Qt6::Core Qt6::Test)
target_compile_features(test_exclusion PRIVATE cxx_std_26)
add_test(NAME test_exclusion COMMAND test_exclusion)

add_executable(test_result_ranker
    test_result_ranker.cpp
    ../src/search/Exclusion.cpp
    ../src/search/FuzzyMatch.cpp
    ../src/search/RankConfig.cpp
    ../src/search/ResultRanker.cpp
    ../src/search/SearchIndex.cpp
    ../src/search/SearchMatcher.cpp
    ../src/search/SearchQuery.cpp
    ../src/core/MusicSort.cpp
    ../src/core/Rating.cpp
)
target_include_directories(test_result_ranker PRIVATE ../src)
target_link_libraries(test_result_ranker PRIVATE Qt6::Core Qt6::Test)
target_compile_features(test_result_ranker PRIVATE cxx_std_26)
add_test(NAME test_result_ranker COMMAND test_result_ranker)

add_executable(test_main_panel_keybindings
    test_main_panel_keybindings.cpp
    ../src/ui/MainPanelKeybindings.cpp
)
target_include_directories(test_main_panel_keybindings PRIVATE ../src)
target_link_libraries(test_main_panel_keybindings PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
target_compile_features(test_main_panel_keybindings PRIVATE cxx_std_26)
add_test(NAME test_main_panel_keybindings COMMAND test_main_panel_keybindings)

add_executable(test_navigation_scroll
    test_navigation_scroll.cpp
    ../src/ui/NavigableTableView.cpp
)
target_include_directories(test_navigation_scroll PRIVATE ../src)
target_link_libraries(test_navigation_scroll PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_navigation_scroll PRIVATE cxx_std_26)
add_test(NAME test_navigation_scroll COMMAND test_navigation_scroll)
set_tests_properties(test_navigation_scroll PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_queue_table
    test_queue_table.cpp
    ../src/ui/QueueKeybindings.cpp
    ../src/ui/QueueStore.cpp
    ../src/ui/QueueTable.cpp
    ../src/ui/NavigableTableView.cpp
    ../src/ui/NeighborColumnResizer.cpp
    ../src/ui/ResponsiveColumnLayout.cpp
    ../src/ui/ResponsiveColumnOptionsDialog.cpp
    ../src/ui/OverlayScrollBar.cpp
    ../src/core/Rating.cpp
    ../src/ui/StarRating.cpp
    ../src/ui/StarRatingDelegate.cpp
    ../src/search/FuzzyMatch.cpp
    ../src/search/SearchMatcher.cpp
    ../src/search/SearchQuery.cpp
)
target_include_directories(test_queue_table PRIVATE ../src)
target_link_libraries(test_queue_table PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_queue_table PRIVATE cxx_std_26)
add_test(NAME test_queue_table COMMAND test_queue_table)
set_tests_properties(test_queue_table PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_responsive_column_layout
    test_responsive_column_layout.cpp
    ../src/ui/ResponsiveColumnLayout.cpp
    ../src/ui/ResponsiveColumnOptionsDialog.cpp
    ../src/ui/TrackTable.cpp
    ../src/ui/DenseTableDelegate.cpp
    ../src/ui/NavigableTableView.cpp
    ../src/ui/NeighborColumnResizer.cpp
    ../src/ui/OverlayScrollBar.cpp
    ../src/ui/StarRating.cpp
    ../src/ui/StarRatingDelegate.cpp
    ../src/core/MusicSort.cpp
    ../src/core/Rating.cpp
)
target_include_directories(test_responsive_column_layout PRIVATE ../src)
target_link_libraries(test_responsive_column_layout PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_responsive_column_layout PRIVATE cxx_std_26)
add_test(NAME test_responsive_column_layout COMMAND test_responsive_column_layout)
set_tests_properties(test_responsive_column_layout PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_keybindings_dialog
    test_keybindings_dialog.cpp
    ../src/ui/KeybindingsDialog.cpp
    ../src/ui/MainPanelKeybindings.cpp
    ../src/ui/FileExplorerKeybindings.cpp
    ../src/ui/QueueKeybindings.cpp
)
target_include_directories(test_keybindings_dialog PRIVATE ../src)
target_link_libraries(test_keybindings_dialog PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_keybindings_dialog PRIVATE cxx_std_26)
add_test(NAME test_keybindings_dialog COMMAND test_keybindings_dialog)
set_tests_properties(test_keybindings_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_album_grid
    test_album_grid.cpp
    ../src/ui/AlbumGrid.cpp
    ../src/ui/AlbumGridDelegate.cpp
    ../src/ui/AlbumArtFallback.cpp
    ../src/ui/OverlayScrollBar.cpp
    ../src/ui/StarRating.cpp
    ../src/core/MusicSort.cpp
    ../src/core/Rating.cpp
    ArtworkCacheStub.cpp
    ../src/scanner/ArtworkCache.h
    ../resources/resources.qrc
)
target_include_directories(test_album_grid PRIVATE ../src ${CMAKE_CURRENT_BINARY_DIR}/../generated)
target_link_libraries(test_album_grid PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Sql Qt6::Test)
target_compile_features(test_album_grid PRIVATE cxx_std_26)
add_test(NAME test_album_grid COMMAND test_album_grid)
set_tests_properties(test_album_grid PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
