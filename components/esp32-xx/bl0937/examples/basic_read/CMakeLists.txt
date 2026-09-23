cmake_minimum_required(VERSION 3.16)

# Point IDF at the repository root so it discovers the bl0937/ component.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bl0937_basic_read)
