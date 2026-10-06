cmake_minimum_required(VERSION 3.28...3.30)

# Harpia records a region of the screen through the crop filter, and that is
# the only filter it uses -- so it is the only one built. (The colour, keying,
# audio and noise-suppression filters, with the RNNoise / SpeexDSP
# dependencies they needed, were removed from this tree.)

add_library(obs-filters MODULE)
add_library(OBS::filters ALIAS obs-filters)

target_sources(obs-filters PRIVATE crop-filter.c obs-filters.c)

target_link_libraries(obs-filters PRIVATE OBS::libobs $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads>)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in obs-filters.rc)
  target_sources(obs-filters PRIVATE obs-filters.rc)
endif()

set_target_properties_obs(obs-filters PROPERTIES FOLDER plugins PREFIX "")
