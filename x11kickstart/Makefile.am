bin_PROGRAMS = x11kickstart
x11kickstart_SOURCES = x11kickstart.c

x11kickstart_CPPFLAGS = $(DEPS_CFLAGS)
x11kickstart_LDADD = $(DEPS_LIBS)

BUILT_SOURCES = verdana.ttf.h bg.png.h bg-add.png.h da.png.h
CLEANFILES = *.png.h *.ttf.h
EXTRA_DIST = verdana.ttf bg-add.png bg-add.svg bg.png da.png

%.png.h: %.png
	$(AM_V_GEN) $(XXD) -i $< > $@

%.ttf.h: %.ttf
	$(AM_V_GEN) $(XXD) -i $< > $@