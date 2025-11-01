bin_PROGRAMS = x11cal
x11cal_SOURCES = x11cal.c

man1_MANS = x11cal.1

x11cal_CPPFLAGS = $(DEPS_CFLAGS)
x11cal_LDADD = $(DEPS_LIBS)

BUILT_SOURCES = bg.png.h verdana.ttf.h
CLEANFILES = *.png.h *.ttf.h
EXTRA_DIST = bg.png verdana.ttf x11cal.1

%.png.h: %.png
	$(AM_V_GEN) $(XXD) -i $< > $@

%.ttf.h: %.ttf
	$(AM_V_GEN) $(XXD) -i $< > $@