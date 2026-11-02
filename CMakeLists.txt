cmake_minimum_required(VERSION 3.16)

# All dependencies are vendored under components/; no registry access.
set(ENV{IDF_COMPONENT_MANAGER} "0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_s3_iot_finder)
