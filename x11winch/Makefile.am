bin_PROGRAMS = x11winch
x11winch_SOURCES = x11winch.c

x11winch_CPPFLAGS = $(DEPS_CFLAGS)
x11winch_LDADD = $(DEPS_LIBS)

BUILT_SOURCES = verdana.ttf.h bg.png.h bg-add.png.h stickey.png.h
CLEANFILES = *.png.h *.ttf.h
EXTRA_DIST = verdana.ttf bg-add.png bg-add.svg bg.png stickey.png

%.png.h: %.png
	$(AM_V_GEN) $(XXD) -i $< > $@

%.ttf.h: %.ttf
	$(AM_V_GEN) $(XXD) -i $< > $@