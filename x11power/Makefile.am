bin_PROGRAMS = x11power
x11power_SOURCES = x11power.c

x11power_CPPFLAGS = $(DEPS_CFLAGS)
x11power_LDADD = $(DEPS_LIBS)

BUILT_SOURCES = verdana.ttf.h bg.png.h gpm-ac-adapter.png.h gpm-brightness.png.h gpm-hibernate.png.h gpm-primary-000-charging.png.h gpm-primary-000.png.h gpm-primary-010-charging.png.h gpm-primary-010.png.h gpm-primary-020-charging.png.h gpm-primary-020.png.h gpm-primary-040-charging.png.h gpm-primary-040.png.h gpm-primary-060-charging.png.h gpm-primary-060.png.h gpm-primary-080-charging.png.h gpm-primary-080.png.h gpm-primary-090-charging.png.h gpm-primary-090.png.h gpm-primary-100-charging.png.h gpm-primary-100.png.h gpm-primary-charged.png.h gpm-primary-missing-charging.png.h gpm-primary-missing.png.h
CLEANFILES = *.png.h *.ttf.h
EXTRA_DIST = verdana.ttf bg.png gpm-ac-adapter.png gpm-brightness.png gpm-hibernate.png gpm-primary-000-charging.png gpm-primary-000.png gpm-primary-010-charging.png gpm-primary-010.png gpm-primary-020-charging.png gpm-primary-020.png gpm-primary-040-charging.png gpm-primary-040.png gpm-primary-060-charging.png gpm-primary-060.png gpm-primary-080-charging.png gpm-primary-080.png gpm-primary-090-charging.png gpm-primary-090.png gpm-primary-100-charging.png gpm-primary-100.png gpm-primary-charged.png gpm-primary-missing-charging.png gpm-primary-missing.png

%.png.h: %.png
	$(AM_V_GEN) $(XXD) -i $< > $@

%.ttf.h: %.ttf
	$(AM_V_GEN) $(XXD) -i $< > $@