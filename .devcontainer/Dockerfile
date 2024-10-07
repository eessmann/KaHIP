FROM mcr.microsoft.com/devcontainers/cpp:1-ubuntu-24.04

ARG REINSTALL_CMAKE_VERSION_FROM_SOURCE="none"

# Optionally install the cmake for vcpkg
COPY ./reinstall-cmake.sh /tmp/

RUN if [ "${REINSTALL_CMAKE_VERSION_FROM_SOURCE}" != "none" ]; then \
        chmod +x /tmp/reinstall-cmake.sh && /tmp/reinstall-cmake.sh ${REINSTALL_CMAKE_VERSION_FROM_SOURCE}; \
    fi \
    && rm -f /tmp/reinstall-cmake.sh

RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y install ubuntu-dbgsym-keyring  \
    && printf "deb http://ddebs.ubuntu.com noble main restricted universe multiverse\ndeb http://ddebs.ubuntu.com noble-updates main restricted universe multiverse\ndeb http://ddebs.ubuntu.com noble-proposed main restricted universe multiverse" | \
    tee -a /etc/apt/sources.list.d/ddebs.list

# [Optional] Uncomment this section to install additional vcpkg ports.
RUN su vscode -c "export VCPKG_FORCE_SYSTEM_BINARIES=arm && ${VCPKG_ROOT}/vcpkg install catch2 fmt metis"

# [Optional] Uncomment this section to install additional packages.
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
     && apt-get -y install gfortran openmpi-bin openmpi-bin-dbgsym libopenmpi-dev libopenmpi3t64 ninja-build clang-tidy cppcheck iwyu
