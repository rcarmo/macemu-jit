# Unix makefile for Basilisk II

## System specific configuration

SHELL = /bin/sh

prefix = /usr/local
exec_prefix = ${prefix}
bindir = ${exec_prefix}/bin
datadir = ${prefix}/share
mandir = ${prefix}/share/man
man1dir = $(mandir)/man1

KEYCODES = ../SDL/keycodes

DESTDIR =

CC = gcc
CXX = g++
GCR = 
CFLAGS = -g -O3 -I/usr/include/SDL2 -D_REENTRANT -I/usr/include/libpng16 -fomit-frame-pointer  -fwrapv
CXXFLAGS = -g -O3 -I/usr/include/SDL2 -D_REENTRANT -I/usr/include/libpng16 -fno-rtti -fomit-frame-pointer -fno-exceptions -fno-rtti  -fno-merge-constants -fno-gcse-sm -fwrapv
CPUINCLUDES_FLAGS=-I../uae_cpu_2021
CPUINCLUDES_FLAGS:=$(CPUINCLUDES_FLAGS:-I%=-I./%)
CPPFLAGS =  -I./../include -I./. -I. -I./../CrossPlatform $(CPUINCLUDES_FLAGS) -I./../slirp
DEFS = -DHAVE_CONFIG_H  -DOS_linux -DDIRECT_ADDRESSING -DCPU_aarch64 -DCPU_AARCH64 -DUNALIGNED_PROFITABLE -DAARCH64_ASSEMBLY -DOPTIMIZED_FLAGS -DUSE_JIT -DJIT -DFPU_MPFR -DUPDATE_UAE -D_REENTRANT -DDATADIR=\"$(datadir)/$(APP)\"
LDFLAGS =  -no-pie 
LIBS = -lm -lrt -lrt  -lSDL2  -lvncserver  -lpng16  -lmpfr -lgmp
SYSSRCS = ../SDL/video_sdl.cpp ../SDL/video_sdl2.cpp ../SDL/video_sdl3.cpp ../SDL/evdev_input.cpp ../SDL/vnc_server.cpp  extfs_unix.cpp serial_unix.cpp ether_unix.cpp Linux/scsi_linux.cpp ../SDL/audio_sdl.cpp ../SDL/audio_sdl3.cpp  ../dummy/prefs_editor_dummy.cpp   main_unix.cpp prefs_unix.cpp ../dummy/clip_dummy.cpp
CPUSRCS = ../uae_cpu_2021/basilisk_glue.cpp ../uae_cpu_2021/memory.cpp ../uae_cpu_2021/newcpu.cpp ../uae_cpu_2021/readcpu.cpp ../uae_cpu_2021/fpu/fpu_mpfr.cpp cpustbl.cpp cpudefs.cpp cpuemu1.cpp cpuemu2.cpp cpuemu3.cpp cpuemu4.cpp cpuemu5.cpp cpuemu6.cpp cpuemu7.cpp cpuemu8.cpp cpuemu1_nf.cpp cpuemu2_nf.cpp cpuemu3_nf.cpp cpuemu4_nf.cpp cpuemu5_nf.cpp cpuemu6_nf.cpp cpuemu7_nf.cpp cpuemu8_nf.cpp compemu1.cpp compemu2.cpp compemu3.cpp compemu4.cpp compemu5.cpp compemu6.cpp compemu7.cpp compemu8.cpp ../uae_cpu_2021/compiler/compemu_support.cpp compstbl.o cpustbl_nf.o cpufunctbl.cpp
BLESS = /usr/bin/true
EXEEXT = 
INSTALL = /usr/bin/install -c
INSTALL_PROGRAM = ${INSTALL}
INSTALL_DATA = ${INSTALL} -m 644

SLIRP_CFLAGS = -fno-strict-aliasing
SLIRP_SRCS =     ../slirp/bootp.c     ../slirp/ip_output.c  ../slirp/tcp_input.c      ../slirp/cksum.c     ../slirp/mbuf.c       ../slirp/tcp_output.c     ../slirp/debug.c     ../slirp/misc.c       ../slirp/tcp_subr.c       ../slirp/if.c        ../slirp/sbuf.c       ../slirp/tcp_timer.c      ../slirp/ip_icmp.c   ../slirp/slirp.c      ../slirp/tftp.c           ../slirp/ip_input.c  ../slirp/socket.c     ../slirp/udp.c
SLIRP_OBJS = $(SLIRP_SRCS:../slirp/%.c=obj/%.o)

USE_BINCUE = no

STANDALONE_GUI = no
GUI_CFLAGS = 
GUI_LIBS = 
GUI_SRCS = ../prefs.cpp prefs_unix.cpp prefs_editor_gtk.cpp ../prefs_items.cpp \
	../user_strings.cpp user_strings_unix.cpp xpram_unix.cpp sys_unix.cpp rpc_unix.cpp

GRESOURCE_SRCS = ui/help-overlay.ui ui/menu.ui ui/prefs-editor.ui
GRESOURCE_XML = ui/basiliskii.gresource.xml

XPLAT_SRCS = ../CrossPlatform/vm_alloc.cpp ../CrossPlatform/sigsegv.cpp ../CrossPlatform/video_blit.cpp

## Files
SRCS = ../main.cpp ../prefs.cpp ../prefs_items.cpp \
    sys_unix.cpp ../rom_patches.cpp ../slot_rom.cpp ../rsrc_patches.cpp \
    ../emul_op.cpp ../macos_util.cpp ../xpram.cpp xpram_unix.cpp ../timer.cpp \
    timer_unix.cpp ../adb.cpp ../serial.cpp ../ether.cpp \
    ../sony.cpp ../disk.cpp ../cdrom.cpp ../scsi.cpp ../video.cpp \
    ../audio.cpp ../extfs.cpp disk_sparsebundle.cpp \
	tinyxml2.cpp \
    ../user_strings.cpp user_strings_unix.cpp sshpty.c strlcpy.c rpc_unix.cpp \
    $(XPLAT_SRCS) $(SYSSRCS) $(CPUSRCS) $(SLIRP_SRCS)
APP_FLAVOR ?=
ifneq ($(APP_FLAVOR),)
	CURR_APP_FLAVOR := -$(APP_FLAVOR)
else
	CURR_APP_FLAVOR :=
endif
APP_BASENAME = BasiliskII
APP = $(APP_BASENAME)$(CURR_APP_FLAVOR)
APP_APP = $(APP).app

PROGS = $(APP)$(EXEEXT)
ifeq ($(STANDALONE_GUI),yes)
GUI_APP = BasiliskIIGUI
GUI_APP_APP = $(GUI_APP).app
PROGS += $(GUI_APP)$(EXEEXT)
else
CXXFLAGS += $(GUI_CFLAGS)
endif

## Rules
.PHONY: modules install installdirs uninstall mostlyclean clean distclean depend dep
.SUFFIXES:
.SUFFIXES: .c .cpp .s .o .h

all: $(PROGS)

OBJ_DIR = obj
$(OBJ_DIR)::
	@[ -d $(OBJ_DIR) ] || mkdir $(OBJ_DIR) > /dev/null 2>&1

define SRCS_LIST_TO_OBJS
	$(addprefix $(OBJ_DIR)/, $(addsuffix .o, $(foreach file, $(SRCS), \
	$(basename $(notdir $(file))))))
endef
OBJS := $(SRCS_LIST_TO_OBJS)
SRCS := $(SRCS:%=./%)

define GUI_SRCS_LIST_TO_OBJS
	$(addprefix $(OBJ_DIR)/, $(addsuffix .guio, $(foreach file, $(GUI_SRCS), \
	$(basename $(notdir $(file))))))
endef
GUI_OBJS = $(GUI_SRCS_LIST_TO_OBJS)
ifeq ($(USE_BINCUE),yes)
GUI_OBJS += bincue.o
endif
GUI_SRCS := $(GUI_SRCS:%=./%)

SRC_PATHS += $(sort $(foreach file, $(SRCS), $(dir $(file))))
VPATH :=
VPATH += $(addprefix :, $(subst  ,:, $(filter-out $($(subst, :, ,$(VPATH))), $(SRC_PATHS))))

## Documentation files
OSX_DOCS = ../MacOSX/Credits.html ../MacOSX/ToDo.html ../MacOSX/HowTo.html ../MacOSX/Versions.html

$(APP)$(EXEEXT): $(OBJ_DIR) $(OBJS)
	$(CXX) -o $@ $(LDFLAGS) $(OBJS) $(LIBS) $(GUI_LIBS)
	$(BLESS) $(APP)$(EXEEXT)

$(GUI_APP)$(EXEEXT): $(OBJ_DIR) $(GUI_OBJS)
	$(CXX) -o $@ $(LDFLAGS) $(GUI_OBJS) $(GUI_LIBS) $(LIBS)

$(APP)_app: $(APP) $(OSX_DOCS) ../../README ../MacOSX/Info.plist ../MacOSX/$(APP).icns
	rm -rf $(APP_APP)/Contents
	mkdir -p $(APP_APP)/Contents
	./cpr.sh ../MacOSX/Info.plist $(APP_APP)/Contents/
	echo -n 'APPL????' > $(APP_APP)/Contents/PkgInfo
	mkdir -p $(APP_APP)/Contents/MacOS
	./cpr.sh $(APP) $(APP_APP)/Contents/MacOS/
	strip -x $(APP_APP)/Contents/MacOS/$(APP)
	mkdir -p $(APP_APP)/Contents/Resources
	./cpr.sh ../MacOSX/$(APP).icns $(APP_APP)/Contents/Resources/
	./cpr.sh ../MacOSX/English.lproj $(APP_APP)/Contents/Resources/
	cp -f $(OSX_DOCS) $(APP_APP)/Contents/Resources/
	cp -f ../../README $(APP_APP)/Contents/Resources/README.txt

$(GUI_APP)_app: $(GUI_APP) ../MacOSX/Info.plist ../MacOSX/$(APP).icns
	rm -rf $(GUI_APP_APP)/Contents
	mkdir -p $(GUI_APP_APP)/Contents
	sed -e "s/$(APP)/$(GUI_APP)/" < ../MacOSX/Info.plist > $(GUI_APP_APP)/Contents/Info.plist
	echo -n 'APPL????' > $(GUI_APP_APP)/Contents/PkgInfo
	mkdir -p $(GUI_APP_APP)/Contents/MacOS
	./cpr.sh $(GUI_APP) $(GUI_APP_APP)/Contents/MacOS/
	strip -x $(GUI_APP_APP)/Contents/MacOS/$(GUI_APP)
	mkdir -p $(GUI_APP_APP)/Contents/Resources
	./cpr.sh ../MacOSX/$(APP).icns $(GUI_APP_APP)/Contents/Resources/$(GUI_APP).icns

modules:
	cd Linux/NetDriver; make

install: $(PROGS) installdirs
	$(INSTALL_PROGRAM) $(APP)$(EXEEXT) $(DESTDIR)$(bindir)/$(APP)$(EXEEXT)
	if test -f "$(GUI_APP)$(EXEEXT)"; then \
	  $(INSTALL_PROGRAM) $(GUI_APP)$(EXEEXT) $(DESTDIR)$(bindir)/$(GUI_APP)$(EXEEXT); \
	fi
	-$(INSTALL_DATA) ./$(APP_BASENAME).1 $(DESTDIR)$(man1dir)/$(APP).1
	$(INSTALL_DATA) ./$(KEYCODES) $(DESTDIR)$(datadir)/$(APP)/keycodes
	$(INSTALL_DATA) ./fbdevices $(DESTDIR)$(datadir)/$(APP)/fbdevices
	$(INSTALL_DATA) ./tunconfig $(DESTDIR)$(datadir)/$(APP)/tunconfig

installdirs:
	$(SHELL) ./mkinstalldirs $(DESTDIR)$(bindir) $(DESTDIR)$(man1dir) $(DESTDIR)$(datadir)/$(APP)

uninstall:
	rm -f $(DESTDIR)$(bindir)/$(APP)$(EXEEXT)
	if test -f "$(GUI_APP_EXE)"; then \
	  rm -f $(DESTDIR)$(bindir)/$(GUI_APP_EXE); \
	fi
	rm -f $(DESTDIR)$(man1dir)/$(APP).1
	rm -f $(DESTDIR)$(datadir)/$(APP)/keycodes
	rm -f $(DESTDIR)$(datadir)/$(APP)/fbdevices
	rm -f $(DESTDIR)$(datadir)/$(APP)/tunconfig
	rmdir $(DESTDIR)$(datadir)/$(APP)

mostlyclean:
	rm -f $(PROGS) $(OBJ_DIR)/* core* *.core *~ *.bak ui/*~ ui/*.bak

clean: mostlyclean
	rm -f cpuemu.cpp cpudefs.cpp cputmp*.s cpufast*.s cpustbl.cpp cputbl.h compemu.cpp compstbl.cpp comptbl.h g_resource.cpp

distclean: clean
	rm -rf $(OBJ_DIR)
	rm -rf autom4te.cache
	rm -f Makefile
	rm -f config.cache config.log config.status config.h
	rm -f Darwin/lowmem Darwin/pagezero

depend dep:
	makedepend $(CPPFLAGS)  # Truncate out previous rules
	# Manually process emitted obj targets to take out extra directory layers
	makedepend $(CPPFLAGS) -pobj/ -Y. $(SRCS) -f- 2>/dev/null | sed 's#^obj/.*/\(.*\.o\):#obj/\1:#' >> Makefile

$(OBJ_DIR)/SDLMain.o : SDLMain.m
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o : ./../slirp/%.c
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) $(SLIRP_CFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o : %.c
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o : %.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o : %.m
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o : %.mm
	$(CXX) $(CPPFLAGS) $(DEFS) $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/%.o : %.s
	$(CC) $(CPPFLAGS) $(DEFS) $(CFLAGS) -c $< -o $@
$(OBJ_DIR)/%.guio : %.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) $(CXXFLAGS) $(GUI_CFLAGS) -DSTANDALONE_GUI -c $< -o $@

# Windows resources
$(OBJ_DIR)/%.o: %.rc
	windres --include-dir ../Windows -i $< -o $@

$(OBJ_DIR)/build68k$(EXEEXT): $(OBJ_DIR)/build68k.o
	$(CC) $(LDFLAGS) -o $(OBJ_DIR)/build68k$(EXEEXT) $(OBJ_DIR)/build68k.o
$(OBJ_DIR)/gencpu$(EXEEXT): $(OBJ_DIR)/gencpu.o $(OBJ_DIR)/readcpu.o $(OBJ_DIR)/cpudefs.o
	$(CXX) $(LDFLAGS) -o $(OBJ_DIR)/gencpu$(EXEEXT) $(OBJ_DIR)/gencpu.o $(OBJ_DIR)/readcpu.o $(OBJ_DIR)/cpudefs.o
$(OBJ_DIR)/gencomp$(EXEEXT): $(OBJ_DIR)/gencomp.o $(OBJ_DIR)/readcpu.o $(OBJ_DIR)/cpudefs.o
	$(CXX) $(LDFLAGS) -o $(OBJ_DIR)/gencomp$(EXEEXT) $(OBJ_DIR)/gencomp.o $(OBJ_DIR)/readcpu.o $(OBJ_DIR)/cpudefs.o

UAE_PATH = ../uae_cpu_2021

cpudefs.cpp: $(OBJ_DIR)/build68k$(EXEEXT) ./$(UAE_PATH)/table68k
	$(OBJ_DIR)/build68k$(EXEEXT) <./$(UAE_PATH)/table68k >cpudefs.cpp
cpustbl.cpp: cpuemu.cpp
cpustbl_nf.cpp: cpustbl.cpp
compstbl.cpp: compemu.cpp
cputbl.h: cpuemu.cpp
comptbl.h: compemu.cpp
cpufunctbl.cpp: cputbl.h

cpuemu.cpp: $(OBJ_DIR)/gencpu$(EXEEXT)
	$(OBJ_DIR)/gencpu$(EXEEXT)

compemu.cpp: $(OBJ_DIR)/gencomp$(EXEEXT)
	$(OBJ_DIR)/gencomp$(EXEEXT)

$(OBJ_DIR)/cpustbl_nf.o: cpustbl.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) $(CXXFLAGS) -DNOFLAGS -c $< -o $@

$(OBJ_DIR)/compemu_support.o: compemu_support.cpp comptbl.h
	$(CXX) $(CPPFLAGS) $(DEFS) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/cpuemu1.o: cpuemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_1 $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/cpuemu2.o: cpuemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_2 $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/cpuemu3.o: cpuemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_3 $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/cpuemu4.o: cpuemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_4 $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/cpuemu5.o: cpuemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_5 $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/cpuemu6.o: cpuemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_6 $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/cpuemu7.o: cpuemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_7 $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/cpuemu8.o: cpuemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_8 $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/cpuemu1_nf.o: cpuemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_1 -DNOFLAGS $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/cpuemu2_nf.o: cpuemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_2 -DNOFLAGS $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/cpuemu3_nf.o: cpuemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_3 -DNOFLAGS $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/cpuemu4_nf.o: cpuemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_4 -DNOFLAGS $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/cpuemu5_nf.o: cpuemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_5 -DNOFLAGS $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/cpuemu6_nf.o: cpuemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_6 -DNOFLAGS $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/cpuemu7_nf.o: cpuemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_7 -DNOFLAGS $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/cpuemu8_nf.o: cpuemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_8 -DNOFLAGS $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/compemu1.o: compemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_1 $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/compemu2.o: compemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_2 $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/compemu3.o: compemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_3 $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/compemu4.o: compemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_4 $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/compemu5.o: compemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_5 $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/compemu6.o: compemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_6 $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/compemu7.o: compemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_7 $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/compemu8.o: compemu.cpp
	$(CXX) $(CPPFLAGS) $(DEFS) -DPART_8 $(CXXFLAGS) -c $< -o $@

g_resource.cpp: $(GRESOURCE_SRCS) $(GRESOURCE_XML)
	$(GCR) --generate-source $(GRESOURCE_XML) --target $@

#-------------------------------------------------------------------------
# DO NOT DELETE THIS LINE -- make depend depends on it.
