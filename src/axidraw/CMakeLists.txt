# SPDX-License-Identifier: GPL-2.0-or-later

target_sources(inkscape_base PRIVATE
  core/plot-orchestrator.h
  core/plot-orchestrator.cpp
  device/grbl-client.h
  device/grbl-client.cpp
  device/serial-port.h
  device/serial-port.cpp
  device/tcp-port.h
  device/tcp-port.cpp
  device/serial-port-scan.h
  device/serial-port-scan.cpp
  pipeline/grbl-export.h
  pipeline/grbl-export.cpp
)
