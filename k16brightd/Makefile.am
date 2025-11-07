dist_pkgdata_DATA =

bin_PROGRAMS =
libexec_PROGRAMS = k16brightd

k16brightd_SOURCES = k16brightd.c
k16brightd_CPPFLAGS = $(DBUS_CFLAGS)
k16brightd_LDADD = $(DBUS_LIBS)

libexecdir = $(exec_prefix)/libexec

dbusservicedir = $(datadir)/dbus-1/system-services
dbusservicedata = net.iczelia.K16BrightD.service

dbuspolicydir = /etc/dbus-1/system.d
dbuspolicydata = net.iczelia.K16BrightD.conf

systemdsystemunitdir =  /etc/systemd/system
systemdsystemunit_DATA = net.iczelia.K16BrightD.systemd.service

install-data-local:
	$(MKDIR_P) $(DESTDIR)$(dbusservicedir)
	$(INSTALL_DATA) $(dbusservicedata) $(DESTDIR)$(dbusservicedir)/
	$(MKDIR_P) $(DESTDIR)$(dbuspolicydir)
	$(INSTALL_DATA) $(dbuspolicydata) $(DESTDIR)$(dbuspolicydir)/
	$(MKDIR_P) $(DESTDIR)$(systemdsystemunitdir)
	$(INSTALL_DATA) net.iczelia.K16BrightD.systemd.service $(DESTDIR)$(systemdsystemunitdir)/net.iczelia.K16BrightD.service

uninstall-hook:
	@echo "Removing D-Bus service and policy..."
	@$(RM) $(DESTDIR)$(dbusservicedir)/$(dbusservicedata)
	@$(RM) $(DESTDIR)$(dbuspolicydir)/$(dbuspolicydata)
	@echo "Removing systemd service..."
	@$(RM) $(DESTDIR)$(systemdsystemunitdir)/net.iczelia.K16BrightD.service || true

install-data-hook:
	@if ! getent group powerctl >/dev/null; then echo "Creating group 'powerctl'..."; groupadd -r "powerctl"; fi
	@echo "You may add users to 'powerctl' with:"
	@echo "  sudo usermod -aG powerctl <username>"

EXTRA_DIST = systemd.service net.iczelia.K16BrightD.conf net.iczelia.K16BrightD.service example.c