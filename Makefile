TARGET := 3DS

NAME := FTB

BUILD_DIR := build
OUTPUT_DIR := output
INCLUDE_DIRS := include
SOURCE_DIRS := source
ROMFS_DIR := romfs

LIBRARY_DIRS += $(DEVKITPRO)/libctru $(DEVKITPRO)/portlibs/armv6k $(DEVKITPRO)/portlibs/3ds
LIBRARIES += curl mbedtls mbedx509 mbedcrypto jansson z citro3d ctru

EXTRA_OUTPUT_FILES := servefiles

# C++ is only the ftpd port (source/ftpd). Upstream builds it as C++20 release with IPv4 only.
BUILD_FLAGS_CXX := -std=gnu++20 -DNDEBUG -DNO_IPV6

# .version/version and .version/versionTag (bump with .scripts/bumpVersion.ps1).
VERSION_PARTS := $(subst ., ,$(firstword $(file < .version/version)))
VERSION_TAG := $(firstword $(file < .version/versionTag))

BUILD_FLAGS := -Wno-format-truncation -DVERSION_TAG=\"$(VERSION_TAG)\"

VERSION_MAJOR := $(word 1, $(VERSION_PARTS))
VERSION_MINOR := $(word 2, $(VERSION_PARTS))
VERSION_MICRO := $(word 3, $(VERSION_PARTS))

# makerom's -major/-minor/-micro pack 6/6/4 bits, capping patch at 15. The CIA gets the raw u16
# X*10000 + Y*100 + Z instead (1.3.5 = 10305); bumpVersion.ps1 keeps minor and patch under 100.
TITLE_VERSION := $(shell echo $$(( $(VERSION_MAJOR) * 10000 + $(VERSION_MINOR) * 100 + $(VERSION_MICRO) )))

DESCRIPTION := Open source title manager.
AUTHOR := Steveice10, fosterbarnes

# Distinct from FBI (CTR-P-CFBI, 0xF8001) so both install side by side.
PRODUCT_CODE := CTR-P-CFTB
UNIQUE_ID := 0xF8F7B

ICON_FLAGS := --flags visible,ratingrequired,recordusage --cero 153 --esrb 153 --usk 153 --pegigen 153 --pegiptr 153 --pegibbfc 153 --cob 153 --grb 153 --cgsrr 153

BANNER_AUDIO := meta/audio_3ds.wav
BANNER_IMAGE := meta/banner_3ds.png
ICON := meta/icon_3ds.png
LOGO := meta/logo_3ds.bcma.lz

# INTERNAL #

include buildtools/make_base

# make_base passes the split version to makerom; swap in the packed one (recipes expand it later).
COMMON_MAKEROM_FLAGS := $(subst -major $(VERSION_MAJOR) -minor $(VERSION_MINOR) -micro $(VERSION_MICRO),-ver $(TITLE_VERSION),$(COMMON_MAKEROM_FLAGS))

# make_base still defines the pre-2.0 libctru macros, which makes 3ds.h warn in every file.
CC_FLAGS := $(subst -DARM11 -D_3DS,-D__3DS__,$(CC_FLAGS))
CXX_FLAGS := $(subst -DARM11 -D_3DS,-D__3DS__,$(CXX_FLAGS))
