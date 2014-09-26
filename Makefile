all: dbus_glib_marshal_navigation.h nm-nav-provider

nm-nav-provider: nm-nav-provider.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags --libs hal dbus-1 glib-2.0 \
	conic navigation gconf-2.0 libxml-2.0 liblocation) $^ -o $@

dbus_glib_marshal_navigation.h: nm-nav-provider.xml
	dbus-binding-tool --mode=glib-server --prefix=navigation $< --output=$@

clean:
	$(RM) *.o dbus_glib_marshal_navigation.h nm-nav-provider

install:
	install -d "$(DESTDIR)/usr/lib/nokiamaps-navigation-provider/"
	install -d "$(DESTDIR)/usr/share/dbus-1/services/"
	install -d "$(DESTDIR)/usr/share/osso-navigation-providers/"
	install -m 755 nm-nav-provider "$(DESTDIR)/usr/lib/nokiamaps-navigation-provider/"
	install -m 644 nm-nav-provider.service "$(DESTDIR)/usr/share/dbus-1/services/"
	install -m 644 nokia-maps.desktop "$(DESTDIR)/usr/share/osso-navigation-providers/"
