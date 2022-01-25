# Makefile

TARGET = hopstepz

OBJS = \
	src/muse/RmmChipMuse.o \
	src/tools/constools.o \
	src/tools/CUTimeCount.o\
	src/tools/tools.o \
	src/CMsxVoidMemory.o \
	src/CHopStepZ.o \
	src/CMsxMusic.o \
	src/CMsxIoSystem.o \
	src/CMsxMemSlotSystem.o \
	src/CRam256k.o \
	src/CScc.o \
	src/CZ80MsxDos.o \
	src/main.o \
	src/playercom.o \
	src/stdafx.o
INCPATH	= \
	-Isrc \
	-Isrc/muse/ \
	-Isrc/tools

CC = gcc
CXX = g++
CFLAGS = -std=c99 -Wall -O2 $(INCPATH)
CFLAGS += -D_UNICODE -DUNICODE
CXXFLAGS = -std=c++11 -Wall -O2 $(INCPATH)
CXXFLAGS += -D_UNICODE -DUNICODE
CXXFLAGS += -DUSE_RAMSXMUSE
CXXFLAGS += -DNDEBUG

LDFLAGS = -pthread -lrt -lz -lwiringPi
LDFLAGS += -Wl,-Map=${TARGET}.map

.PHONY: all
all: $(TARGET)

.PHONY: clean
clean:
	$(RM) $(OBJS) $(TARGET) $(TARGET).map

.PHONY: ver
ver:
	$(CXX) --version

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) $(LDFLAGS) -o $(TARGET)

