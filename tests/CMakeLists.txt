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

add_executable(test_track_info_settings
    test_track_info_settings.cpp
    ../src/core/HumanQuantity.cpp
    ../src/core/Rating.cpp
    ../src/ui/trackinfo/TrackInfoSettings.cpp
)
target_include_directories(test_track_info_settings PRIVATE ../src)
target_link_libraries(test_track_info_settings PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
target_compile_features(test_track_info_settings PRIVATE cxx_std_26)
add_test(NAME test_track_info_settings COMMAND test_track_info_settings)
set_tests_properties(test_track_info_settings PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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
    ../src/app/AppPaths.cpp
    ../src/core/GenreTags.cpp
    ../src/core/MetadataBlob.cpp
    ../src/core/Rating.cpp
    ../src/db/Database.cpp
    ../src/fs/PathResolver.cpp
    ../src/search/IndexCache.cpp
    ../src/search/fold/Fold.cpp
)
target_include_directories(test_schema PRIVATE ../src)
target_link_libraries(test_schema PRIVATE Qt6::Core Qt6::Sql Qt6::Test PkgConfig::ZSTD)
target_compile_features(test_schema PRIVATE cxx_std_26)
add_test(NAME test_schema COMMAND test_schema)

# Standalone CLI search path (build/load/refresh the cache, query the engine).
add_executable(test_search_cli
    test_search_cli.cpp
    ../src/app/AppPaths.cpp
    ../src/cli/SearchCli.cpp
    ../src/core/GenreTags.cpp
    ../src/core/MetadataBlob.cpp
    ../src/core/Rating.cpp
    ../src/db/Database.cpp
    ../src/fs/PathResolver.cpp
    ../src/core/MusicSort.cpp
    ../src/search/Exclusion.cpp
    ../src/search/FuzzyMatch.cpp
    ../src/search/IndexCache.cpp
    ../src/search/SearchIndex.cpp
    ../src/search/SearchMatcher.cpp
    ../src/search/SearchQuery.cpp
    ../src/search/fold/Fold.cpp
)
target_include_directories(test_search_cli PRIVATE ../src)
target_link_libraries(test_search_cli PRIVATE Qt6::Core Qt6::Sql Qt6::Test PkgConfig::ZSTD)
target_compile_features(test_search_cli PRIVATE cxx_std_26)
add_test(NAME test_search_cli COMMAND test_search_cli)

add_executable(test_playlist_database
    test_playlist_database.cpp
    ../src/db/PlaylistDatabase.cpp
)
target_include_directories(test_playlist_database PRIVATE ../src)
target_link_libraries(test_playlist_database PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
target_compile_features(test_playlist_database PRIVATE cxx_std_26)
add_test(NAME test_playlist_database COMMAND test_playlist_database)
set_tests_properties(test_playlist_database PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_listen_history
    test_listen_history.cpp
    ../src/scrobble/ListenHistoryStore.cpp
)
target_include_directories(test_listen_history PRIVATE ../src)
target_link_libraries(test_listen_history PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
target_compile_features(test_listen_history PRIVATE cxx_std_26)
add_test(NAME test_listen_history COMMAND test_listen_history)
set_tests_properties(test_listen_history PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_feature_store
    test_feature_store.cpp
    ../src/features/FeatureStore.cpp
)
target_include_directories(test_feature_store PRIVATE ../src)
target_link_libraries(test_feature_store PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
target_compile_features(test_feature_store PRIVATE cxx_std_26)
add_test(NAME test_feature_store COMMAND test_feature_store)
set_tests_properties(test_feature_store PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_song_identity
    test_song_identity.cpp
    ../src/features/SongIdentity.cpp
)
target_include_directories(test_song_identity PRIVATE ../src)
target_link_libraries(test_song_identity PRIVATE Qt6::Core Qt6::Test)
target_compile_features(test_song_identity PRIVATE cxx_std_26)
add_test(NAME test_song_identity COMMAND test_song_identity)

add_executable(test_quality_rank
    test_quality_rank.cpp
    ../src/features/QualityRank.cpp
)
target_include_directories(test_quality_rank PRIVATE ../src)
target_link_libraries(test_quality_rank PRIVATE Qt6::Core Qt6::Test)
target_compile_features(test_quality_rank PRIVATE cxx_std_26)
add_test(NAME test_quality_rank COMMAND test_quality_rank)

add_executable(test_play_events
    test_play_events.cpp
    ../src/scrobble/ListenHistoryStore.cpp
    ../src/scrobble/PlayEventRecorder.cpp
)
target_include_directories(test_play_events PRIVATE ../src)
target_link_libraries(test_play_events PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
target_compile_features(test_play_events PRIVATE cxx_std_26)
add_test(NAME test_play_events COMMAND test_play_events)
set_tests_properties(test_play_events PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_radio
    test_radio.cpp
    ../src/app/AppPaths.cpp
    ../src/core/GenreTags.cpp
    ../src/core/MetadataBlob.cpp
    ../src/core/Rating.cpp
    ../src/db/Database.cpp
    ../src/fs/PathResolver.cpp
    ../src/reco/AffinityPool.cpp
    ../src/reco/ArtistRadio.cpp
    ../src/reco/RadioFilters.cpp
    ../src/reco/RadioMix.cpp
    ../src/reco/RadioSession.cpp
    ../src/reco/ReasonText.cpp
    ../src/reco/TrackScorer.cpp
    ../src/scrobble/ListenHistoryStore.cpp
    ../src/search/IndexCache.cpp
    ../src/search/fold/Fold.cpp
)
target_include_directories(test_radio PRIVATE ../src)
target_link_libraries(test_radio PRIVATE Qt6::Core Qt6::Sql Qt6::Test PkgConfig::ZSTD)
target_compile_features(test_radio PRIVATE cxx_std_26)
add_test(NAME test_radio COMMAND test_radio)
set_tests_properties(test_radio PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_scrobble_backfill
    test_scrobble_backfill.cpp
    ../src/scrobble/ListenHistoryStore.cpp
    ../src/scrobble/BackfillParse.cpp
    ../src/scrobble/ScrobbleBackfill.cpp
)
target_include_directories(test_scrobble_backfill PRIVATE ../src)
target_link_libraries(test_scrobble_backfill PRIVATE Qt6::Core Qt6::Network Qt6::Sql Qt6::Test)
target_compile_features(test_scrobble_backfill PRIVATE cxx_std_26)
add_test(NAME test_scrobble_backfill COMMAND test_scrobble_backfill)
set_tests_properties(test_scrobble_backfill PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_path_guesser
    test_path_guesser.cpp
    ../src/scanner/PathMetadataGuesser.cpp
)
target_include_directories(test_path_guesser PRIVATE ../src)
target_link_libraries(test_path_guesser PRIVATE Qt6::Core Qt6::Test)
target_compile_features(test_path_guesser PRIVATE cxx_std_26)
add_test(NAME test_path_guesser COMMAND test_path_guesser)

add_executable(test_scan_pipeline
    test_scan_pipeline.cpp
    ../src/scanner/ScanPipeline.cpp
    ../src/scanner/PathMetadataGuesser.cpp
    ../src/scanner/DirectoryWalker.cpp
    ../src/scanner/LibraryScanner.cpp
    ../src/scanner/TagReader.cpp
    ../src/scanner/TagRating.cpp
    ../src/core/GenreTags.cpp
    ../src/core/MetadataBlob.cpp
    ../src/core/Rating.cpp
    ../src/db/Database.cpp
    ../src/fs/PathResolver.cpp
    ../src/search/fold/Fold.cpp
)
target_include_directories(test_scan_pipeline PRIVATE ../src)
target_link_libraries(test_scan_pipeline PRIVATE Qt6::Core Qt6::Sql Qt6::Test PkgConfig::ZSTD PkgConfig::TAGLIB)
target_compile_features(test_scan_pipeline PRIVATE cxx_std_26)
add_test(NAME test_scan_pipeline COMMAND test_scan_pipeline)

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
    ../src/search/fold/Fold.cpp
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

add_executable(test_fold
    test_fold.cpp
    ../src/search/fold/Fold.cpp
)
target_include_directories(test_fold PRIVATE ../src)
target_link_libraries(test_fold PRIVATE Qt6::Core Qt6::Test)
target_compile_features(test_fold PRIVATE cxx_std_26)
add_test(NAME test_fold COMMAND test_fold)

add_executable(test_search_index
    test_search_index.cpp
    ../src/core/MusicSort.cpp
    ../src/search/Exclusion.cpp
    ../src/search/FuzzyMatch.cpp
    ../src/search/SearchIndex.cpp
    ../src/search/SearchMatcher.cpp
    ../src/search/SearchQuery.cpp
    ../src/search/fold/Fold.cpp
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
    ../src/search/fold/Fold.cpp
)
target_include_directories(test_search_matcher PRIVATE ../src)
target_link_libraries(test_search_matcher PRIVATE Qt6::Core Qt6::Test)
target_compile_features(test_search_matcher PRIVATE cxx_std_26)
add_test(NAME test_search_matcher COMMAND test_search_matcher)

add_executable(test_rank_config
    test_rank_config.cpp
    ../src/core/MusicSort.cpp
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
    ../src/core/MusicSort.cpp
    ../src/search/Exclusion.cpp
    ../src/search/FuzzyMatch.cpp
    ../src/search/SearchIndex.cpp
    ../src/search/SearchMatcher.cpp
    ../src/search/SearchQuery.cpp
    ../src/search/fold/Fold.cpp
)
target_include_directories(test_exclusion PRIVATE ../src)
target_link_libraries(test_exclusion PRIVATE Qt6::Core Qt6::Test)
target_compile_features(test_exclusion PRIVATE cxx_std_26)
add_test(NAME test_exclusion COMMAND test_exclusion)

add_executable(test_result_ranker
    test_result_ranker.cpp
    ../src/core/MusicSort.cpp
    ../src/search/Exclusion.cpp
    ../src/search/FuzzyMatch.cpp
    ../src/search/RankConfig.cpp
    ../src/search/ResultRanker.cpp
    ../src/search/SearchIndex.cpp
    ../src/search/SearchMatcher.cpp
    ../src/search/SearchQuery.cpp
    ../src/search/fold/Fold.cpp
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
    ../src/ui/RowReorderSupport.cpp
)
target_include_directories(test_navigation_scroll PRIVATE ../src)
target_link_libraries(test_navigation_scroll PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_navigation_scroll PRIVATE cxx_std_26)
add_test(NAME test_navigation_scroll COMMAND test_navigation_scroll)
set_tests_properties(test_navigation_scroll PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_panel_border_style
    test_panel_border_style.cpp
)
target_include_directories(test_panel_border_style PRIVATE ../src)
target_link_libraries(test_panel_border_style PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_panel_border_style PRIVATE cxx_std_26)
add_test(NAME test_panel_border_style COMMAND test_panel_border_style)

add_executable(test_queue_table
    test_queue_table.cpp
    ../src/ui/QueueKeybindings.cpp
    ../src/ui/QueueStore.cpp
    ../src/ui/QueueTable.cpp
    ../src/ui/PanelSearchBar.cpp
    ../src/ui/NavigableTableView.cpp
    ../src/ui/RowReorderSupport.cpp
    ../src/ui/NeighborColumnResizer.cpp
    ../src/ui/ResponsiveColumnLayout.cpp
    ../src/ui/ResponsiveColumnOptionsDialog.cpp
    ../src/ui/OverlayScrollBar.cpp
    ../src/core/HumanQuantity.cpp
    ../src/core/Rating.cpp
    ../src/core/TrackDisplay.cpp
    ../src/ui/StarRating.cpp
    ../src/ui/StarRatingDelegate.cpp
    ../src/search/FuzzyMatch.cpp
    ../src/search/SearchMatcher.cpp
    ../src/search/SearchQuery.cpp
    ../src/search/fold/Fold.cpp
)
target_include_directories(test_queue_table PRIVATE ../src)
target_link_libraries(test_queue_table PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_queue_table PRIVATE cxx_std_26)
add_test(NAME test_queue_table COMMAND test_queue_table)
set_tests_properties(test_queue_table PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_playlist_view
    test_playlist_view.cpp
    ../src/db/PlaylistDatabase.cpp
    ../src/ui/PlaylistView.cpp
    ../src/ui/IdleReleaseController.cpp
    ../src/ui/PanelSearchBar.cpp
    ../src/search/SearchMatcher.cpp
    ../src/search/SearchQuery.cpp
    ../src/search/FuzzyMatch.cpp
    ../src/search/fold/Fold.cpp
    ../src/ui/DenseTableDelegate.cpp
    ../src/ui/NavigableTableView.cpp
    ../src/ui/RowReorderSupport.cpp
    ../src/ui/NeighborColumnResizer.cpp
    ../src/ui/ResponsiveColumnLayout.cpp
    ../src/ui/ResponsiveColumnOptionsDialog.cpp
    ../src/ui/OverlayScrollBar.cpp
    ../src/ui/StarRating.cpp
    ../src/ui/StarRatingDelegate.cpp
    ../src/core/Rating.cpp
)
target_include_directories(test_playlist_view PRIVATE ../src)
target_link_libraries(test_playlist_view PRIVATE Qt6::Core Qt6::Sql Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_playlist_view PRIVATE cxx_std_26)
add_test(NAME test_playlist_view COMMAND test_playlist_view)
set_tests_properties(test_playlist_view PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_responsive_column_layout
    test_responsive_column_layout.cpp
    ../src/ui/ResponsiveColumnLayout.cpp
    ../src/ui/ResponsiveColumnOptionsDialog.cpp
    ../src/ui/TrackTable.cpp
    ../src/ui/DenseTableDelegate.cpp
    ../src/ui/NavigableTableView.cpp
    ../src/ui/RowReorderSupport.cpp
    ../src/ui/NeighborColumnResizer.cpp
    ../src/ui/OverlayScrollBar.cpp
    ../src/ui/StarRating.cpp
    ../src/ui/StarRatingDelegate.cpp
    ../src/core/HumanQuantity.cpp
    ../src/core/MusicSort.cpp
    ../src/core/Rating.cpp
    ../src/core/TrackDisplay.cpp
    ../src/search/fold/Fold.cpp
)
target_include_directories(test_responsive_column_layout PRIVATE ../src)
target_link_libraries(test_responsive_column_layout PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_responsive_column_layout PRIVATE cxx_std_26)
add_test(NAME test_responsive_column_layout COMMAND test_responsive_column_layout)
set_tests_properties(test_responsive_column_layout PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_playlist_import
    test_playlist_import.cpp
    ../src/playlist/PlaylistImport.cpp
    ../src/playlist/PlaylistMatcher.cpp
    ../src/playlist/import/YouTubeImport.cpp
    ../src/core/MusicSort.cpp
    ../src/search/Exclusion.cpp
    ../src/search/FuzzyMatch.cpp
    ../src/search/SearchIndex.cpp
    ../src/search/SearchMatcher.cpp
    ../src/search/SearchQuery.cpp
    ../src/search/fold/Fold.cpp
)
target_include_directories(test_playlist_import PRIVATE ../src)
target_link_libraries(test_playlist_import PRIVATE Qt6::Core Qt6::Test)
target_compile_features(test_playlist_import PRIVATE cxx_std_26)
add_test(NAME test_playlist_import COMMAND test_playlist_import)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_playlist_converter
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_playlist_converter.py
            ${CMAKE_SOURCE_DIR}/tools/playlist-import/muzaiten-import)

add_executable(test_keybindings_dialog
    test_keybindings_dialog.cpp
    ../src/ui/KeybindingsDialog.cpp
    ../src/ui/MainPanelKeybindings.cpp
    ../src/ui/FileExplorerKeybindings.cpp
    ../src/ui/QueueKeybindings.cpp
    ../src/ui/ViewKeyReferences.cpp
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
    ../src/search/fold/Fold.cpp
    ArtworkCacheStub.cpp
    ../src/scanner/ArtworkCache.h
    ../resources/resources.qrc
)
target_include_directories(test_album_grid PRIVATE ../src ${CMAKE_CURRENT_BINARY_DIR}/../generated)
target_link_libraries(test_album_grid PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Sql Qt6::Test)
target_compile_features(test_album_grid PRIVATE cxx_std_26)
add_test(NAME test_album_grid COMMAND test_album_grid)
set_tests_properties(test_album_grid PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_music_explorer_view
    test_music_explorer_view.cpp
    ../src/ui/MusicExplorerView.cpp
    ../src/ui/TrackTable.cpp
    ../src/ui/DenseTableDelegate.cpp
    ../src/ui/NavigableTableView.cpp
    ../src/ui/RowReorderSupport.cpp
    ../src/ui/NeighborColumnResizer.cpp
    ../src/ui/ResponsiveColumnLayout.cpp
    ../src/ui/ResponsiveColumnOptionsDialog.cpp
    ../src/ui/OverlayScrollBar.cpp
    ../src/ui/AlbumArtFallback.cpp
    ../src/ui/StarRating.cpp
    ../src/ui/StarRatingDelegate.cpp
    ../src/core/HumanQuantity.cpp
    ../src/core/MusicSort.cpp
    ../src/core/Rating.cpp
    ../src/core/TrackDisplay.cpp
    ../src/search/FuzzyMatch.cpp
    ../src/search/SearchMatcher.cpp
    ../src/search/SearchQuery.cpp
    ../src/search/fold/Fold.cpp
    ArtworkCacheStub.cpp
    ../src/scanner/ArtworkCache.h
    ../resources/resources.qrc
)
target_include_directories(test_music_explorer_view PRIVATE ../src ${CMAKE_CURRENT_BINARY_DIR}/../generated)
target_link_libraries(test_music_explorer_view PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Sql Qt6::Test)
target_compile_features(test_music_explorer_view PRIVATE cxx_std_26)
add_test(NAME test_music_explorer_view COMMAND test_music_explorer_view)
set_tests_properties(test_music_explorer_view PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_panel_search_bar
    test_panel_search_bar.cpp
    ../src/ui/PanelSearchBar.cpp
    ../src/search/SearchMatcher.cpp
    ../src/search/SearchQuery.cpp
    ../src/search/FuzzyMatch.cpp
    ../src/search/fold/Fold.cpp
)
target_include_directories(test_panel_search_bar PRIVATE ../src ${CMAKE_CURRENT_BINARY_DIR}/../generated)
target_link_libraries(test_panel_search_bar PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_panel_search_bar PRIVATE cxx_std_26)
add_test(NAME test_panel_search_bar COMMAND test_panel_search_bar)
set_tests_properties(test_panel_search_bar PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_panel_search
    test_panel_search.cpp
    ../src/ui/PanelSearchController.cpp
    ../src/ui/MainPanelKeybindings.cpp
    ../src/ui/NavigableTableView.cpp
    ../src/ui/RowReorderSupport.cpp
    ../src/search/SearchMatcher.cpp
    ../src/search/SearchQuery.cpp
    ../src/search/FuzzyMatch.cpp
    ../src/search/fold/Fold.cpp
)
target_include_directories(test_panel_search PRIVATE ../src ${CMAKE_CURRENT_BINARY_DIR}/../generated)
target_link_libraries(test_panel_search PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_panel_search PRIVATE cxx_std_26)
add_test(NAME test_panel_search COMMAND test_panel_search)
set_tests_properties(test_panel_search PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_player_core
    test_player_core.cpp
    ../src/player/PlayerCore.cpp
    ../src/playback/PlaybackBackend.h
)
target_include_directories(test_player_core PRIVATE ../src)
target_link_libraries(test_player_core PRIVATE Qt6::Core Qt6::Test)
target_compile_features(test_player_core PRIVATE cxx_std_26)
add_test(NAME test_player_core COMMAND test_player_core)
set_tests_properties(test_player_core PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_ipc_server
    test_ipc_server.cpp
    ../src/ipc/IpcServer.cpp
    ../src/ipc/IpcSocket.cpp
    ../src/app/AppPaths.cpp
)
target_include_directories(test_ipc_server PRIVATE ../src)
target_link_libraries(test_ipc_server PRIVATE Qt6::Core Qt6::Network Qt6::Test)
target_compile_features(test_ipc_server PRIVATE cxx_std_26)
add_test(NAME test_ipc_server COMMAND test_ipc_server)
set_tests_properties(test_ipc_server PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_idle_release_controller
    test_idle_release_controller.cpp
    ../src/ui/IdleReleaseController.cpp
)
target_include_directories(test_idle_release_controller PRIVATE ../src)
target_link_libraries(test_idle_release_controller PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_features(test_idle_release_controller PRIVATE cxx_std_26)
add_test(NAME test_idle_release_controller COMMAND test_idle_release_controller)
set_tests_properties(test_idle_release_controller PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_main_window_navigation test_main_window_navigation.cpp)
target_link_libraries(test_main_window_navigation PRIVATE muzaiten_app_objects Qt6::Test)
target_compile_features(test_main_window_navigation PRIVATE cxx_std_26)
add_test(NAME test_main_window_navigation COMMAND test_main_window_navigation)
set_tests_properties(test_main_window_navigation PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Offline matcher benchmark tool (dev only; not a registered test). Needs a real
# library.sqlite copy: ./playlist_match_bench <library.sqlite> <playlist.jsonl ...>
# Off by default so a normal build pays nothing; enable with
#   cmake -DMUZAITEN_BUILD_MATCH_BENCH=ON
option(MUZAITEN_BUILD_MATCH_BENCH "Build the offline playlist-matcher benchmark tool" OFF)
if(MUZAITEN_BUILD_MATCH_BENCH)
add_executable(playlist_match_bench
    ../tools/playlist-import/match_bench.cpp
    ../src/app/AppPaths.cpp
    ../src/core/GenreTags.cpp
    ../src/core/MetadataBlob.cpp
    ../src/core/Rating.cpp
    ../src/db/Database.cpp
    ../src/fs/PathResolver.cpp
    ../src/playlist/PlaylistImport.cpp
    ../src/playlist/PlaylistMatcher.cpp
    ../src/core/MusicSort.cpp
    ../src/search/Exclusion.cpp
    ../src/search/FuzzyMatch.cpp
    ../src/search/IndexCache.cpp
    ../src/search/RankConfig.cpp
    ../src/search/SearchIndex.cpp
    ../src/search/SearchMatcher.cpp
    ../src/search/SearchQuery.cpp
    ../src/search/fold/Fold.cpp
)
target_include_directories(playlist_match_bench PRIVATE ../src)
target_link_libraries(playlist_match_bench PRIVATE Qt6::Core Qt6::Sql PkgConfig::ZSTD)
target_compile_features(playlist_match_bench PRIVATE cxx_std_26)
endif()
