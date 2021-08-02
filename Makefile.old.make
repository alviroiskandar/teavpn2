#
# SPDX-License-Identifier: GPL-2.0
#
# @author Ammar Faizi <ammarfaizi2@gmail.com> https://www.facebook.com/ammarfaizi2
# @license GNU GPL-2.0
#
# TeaVPN2 - Free VPN Software
#
# Copyright (C) 2021  Ammar Faizi
#

VERSION	= 0
PATCHLEVEL = 1
SUBLEVEL = 2
EXTRAVERSION = -rc1
NAME = Green Grass
TARGET_BIN = teavpn2
PACKAGE_NAME = $(TARGET_BIN)-$(VERSION).$(PATCHLEVEL).$(SUBLEVEL)$(EXTRAVERSION)

export IO_URING_SUPPORT=1


#
# Bin
#
AS	:= as
CC 	:= clang
CXX	:= clang++
LD	:= $(CXX)
VG	:= valgrind
RM	:= rm
MKDIR	:= mkdir
STRIP	:= strip
OBJCOPY	:= objcopy
OBJDUMP	:= objdump
READELF	:= readelf
HOSTCC	:= $(CC)
HOSTCXX	:= $(CXX)


# Flag to link any library to $(TARGET_BIN)
# (middle argumets)
LDFLAGS		:= -ggdb3


# Flag to link any library to $(TARGET_BIN)
# (end arguments)
LIB_LDFLAGS	:= -lpthread


# Flags that only apply to C
CFLAGS		:= -std=c11


# Flags that only apply to C++
CXXFLAGS	:= -std=c++2a


# Flags that only apply to PIC objects.
PIC_FLAGS	:= -fPIC -fpic


# Flags that only apply to PIE objects.
PIE_FLAGS	:= -fPIE -fpie


# `C_CXX_FLAGS` will be appended to `CFLAGS` and `CXXFLAGS`.
C_CXX_FLAGS := \
	-ggdb3 \
	-fstrict-aliasing \
	-fstack-protector-strong \
	-fno-omit-frame-pointer \
	-fdata-sections \
	-ffunction-sections \
	-pedantic-errors \
	-D_GNU_SOURCE \
	-DVERSION=$(VERSION) \
	-DPATCHLEVEL=$(PATCHLEVEL) \
	-DSUBLEVEL=$(SUBLEVEL) \
	-DEXTRAVERSION="\"$(EXTRAVERSION)\"" \
	-DNAME="\"$(NAME)\""



C_CXX_FLAGS_RELEASE := -DNDEBUG
C_CXX_FLAGS_DEBUG :=


# Valgrind flags
VGFLAGS	:= \
	--leak-check=full \
	--show-leak-kinds=all \
	--track-origins=yes \
	--track-fds=yes \
	--error-exitcode=99 \
	-s


ifndef DEFAULT_OPTIMIZATION
	DEFAULT_OPTIMIZATION := -O0
endif


STACK_USAGE_SIZE := 2097152


GCC_WARN_FLAGS := \
	-Wall \
	-Wextra \
	-Wformat \
	-Wformat-security \
	-Wformat-signedness \
	-Wsequence-point \
	-Wstrict-aliasing=3 \
	-Wstack-usage=$(STACK_USAGE_SIZE) \
	-Wunsafe-loop-optimizations


CLANG_WARN_FLAGS := \
	-Wall \
	-Wextra \
	-Weverything \
	-Wno-padded \
	-Wno-unused-macros \
	-Wno-covered-switch-default \
	-Wno-disabled-macro-expansion \
	-Wno-language-extension-token \
	-Wno-used-but-marked-unused


BASE_DIR	:= $(dir $(realpath $(lastword $(MAKEFILE_LIST))))
BASE_DIR	:= $(strip $(patsubst %/, %, $(BASE_DIR)))
BASE_DEP_DIR	:= $(BASE_DIR)/.deps
MAKEFILE_FILE	:= $(lastword $(MAKEFILE_LIST))
INCLUDE_DIR	= -I$(BASE_DIR)


ifneq ($(words $(subst :, ,$(BASE_DIR))), 1)
$(error Source directory cannot contain spaces or colons)
endif


include $(BASE_DIR)/src/build/flags.make
include $(BASE_DIR)/src/build/print.make


#######################################
# Force these variables to be a simple variable
OBJ_CC		:=
OBJ_PRE_CC	:=
OBJ_TMP_CC	:=
OBJ_JUST_RM	:=
SHARED_LIB	:=
OBJ_EXTRA	:=
#######################################


all: $(TARGET_BIN)

include $(BASE_DIR)/src/Makefile

#
# Create dependency directories
#
$(DEP_DIRS):
	$(MKDIR_PRINT)
	$(Q)$(MKDIR) -p $(@)


#
# Add more dependency chain to objects that are not
# compiled from the main Makefile (main Makefile is *this* Makefile).
#
$(OBJ_CC): $(MAKEFILE_FILE) | $(DEP_DIRS)
$(OBJ_PRE_CC): $(MAKEFILE_FILE) | $(DEP_DIRS)


#
# Compile object from main Makefile (main Makefile is *this* Makefile).
#
$(OBJ_CC):
	$(CC_PRINT)
	$(Q)$(CC) $(PIE_FLAGS) $(DEPFLAGS) $(CFLAGS) -c $(O_TO_C) -o $(@)


#
# Include generated dependencies
#
-include $(OBJ_CC:$(BASE_DIR)/%.o=$(BASE_DEP_DIR)/%.d)
-include $(OBJ_PRE_CC:$(BASE_DIR)/%.o=$(BASE_DEP_DIR)/%.d)


#
# Link the target bin.
#
$(TARGET_BIN): $(OBJ_CC) $(OBJ_PRE_CC) $(FBT_CC_OBJ) $(OBJ_EXTRA)
	$(LD_PRINT)
	$(Q)$(LD) $(PIE_FLAGS) $(LDFLAGS) $(^) -o "$(@)" $(LIB_LDFLAGS)


#
# Clean project and also clean bluetea framework objects.
#
clean: bluetea_clean
	$(Q)$(RM) -vrf $(OBJ_JUST_RM) $(TARGET_BIN) $(DEP_DIRS) $(OBJ_CC) $(OBJ_PRE_CC)


clean_all: clean ext_clean

.PHONY: all clean clean_all