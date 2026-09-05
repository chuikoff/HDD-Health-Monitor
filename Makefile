# ============================================================================
#  DriveMonitor - MinGW build
#  Fork of HDDHealth Monitor (Ari Sohandri Putra / ARImetic Inc., MIT)
#  Fork changes: chuikoff
# ============================================================================

ifeq ($(OS),Windows_NT)
    CC      = g++
    WINDRES = windres
else
    CC      = x86_64-w64-mingw32-g++
    WINDRES = x86_64-w64-mingw32-windres
endif

SRCDIR  = src
OBJDIR  = obj
OUTDIR  = bin
TARGET  = $(OUTDIR)/DriveMonitor.exe
SRCS    = $(SRCDIR)/main.cpp \
          $(SRCDIR)/mainwnd.cpp \
          $(SRCDIR)/smart.cpp

OBJS    = $(patsubst $(SRCDIR)/%.cpp, $(OBJDIR)/%.o, $(SRCS))
RES_O   = $(OBJDIR)/app_res.o

# Compiler flags:
#   -mwindows          : GUI subsystem (no console window)
#   -O2                : Optimized release build
#   -DWIN32 ...        : Win32 platform defines expected by the source
#   -Wall              : Enable common warnings; unused-* only are suppressed
#                        (format and C++ type issues are fixed in source).
#   V=1                : Show full compiler/linker commands (default is quiet).
CFLAGS  = -mwindows -O2 \
          -DWIN32 -D_WIN32 -D_WINDOWS -DNDEBUG \
          -I$(SRCDIR) \
          -Wall -Wno-unused-function -Wno-unused-parameter \
          -Wno-unused-variable \
          -finput-charset=UTF-8

# Linker flags:
#   -static*           : Statically link the C/C++ runtime so the .exe
#                        has no external runtime DLL dependencies.
#   -lcomctl32 ...     : Win32 system libraries used by the UI and by
#                        the S.M.A.R.T. / SetupAPI code paths.
LDFLAGS = -mwindows \
          -static -static-libgcc -static-libstdc++ \
          -lcomctl32 -lcomdlg32 -lshell32 \
          -luser32 -lgdi32 -lkernel32 -ladvapi32 -lole32 -luuid \
          -lgdiplus -lshlwapi -lsetupapi -lcfgmgr32


.PHONY: all clean

all: $(OUTDIR) $(OBJDIR) $(TARGET)
	@echo ""
	@echo "  Build complete: $(TARGET)"
	@echo "  100% Free Open Source Software - have fun!"
	@echo ""

# V=1 prints full compiler/linker commands; default is quiet.
ifeq ($(V),1)
Q :=
else
Q := @
endif

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp | $(OBJDIR)
	@echo "  CC  $<"
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

# Resource compilation: the .rc file references the .manifest and .ico,
# so the .o depends on all three.
$(RES_O): $(SRCDIR)/app.rc $(SRCDIR)/app.manifest $(SRCDIR)/app.ico | $(OBJDIR)
	@echo "  RC  $(SRCDIR)/app.rc"
	$(Q)$(WINDRES) --include-dir=$(SRCDIR) $(SRCDIR)/app.rc -o $(RES_O)

$(TARGET): $(OBJS) $(RES_O) | $(OUTDIR)
	@echo "  LD  $@"
	$(Q)$(CC) $(OBJS) $(RES_O) $(LDFLAGS) -o $@

$(OBJDIR):
ifeq ($(OS),Windows_NT)
	@if not exist $(OBJDIR) mkdir $(OBJDIR)
else
	@mkdir -p $(OBJDIR)
endif

$(OUTDIR):
ifeq ($(OS),Windows_NT)
	@if not exist $(OUTDIR) mkdir $(OUTDIR)
else
	@mkdir -p $(OUTDIR)
endif

clean:
ifeq ($(OS),Windows_NT)
	@if exist $(OBJDIR) rmdir /s /q $(OBJDIR)
	@if exist $(OUTDIR) rmdir /s /q $(OUTDIR)
else
	@rm -rf $(OBJDIR) $(OUTDIR)
endif
	@echo "  Clean."
