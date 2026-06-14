CC       = gcc
CFLAGS   = -Wall -Wextra -O2 $(shell pkg-config --cflags gtk+-3.0)
LIBS     = $(shell pkg-config --libs gtk+-3.0)

TARGET   = speed_limit_gui
SRC      = speed_limit_gui.c

# Also keep the original CLI tool building
CLI_TARGET = speed_limit
CLI_SRC    = speed_limit.c

# Install locations
PREFIX     = /usr/local
BINDIR     = $(PREFIX)/bin
ICONDIR    = /usr/share/icons/hicolor/scalable/apps
DESKTOPDIR = /usr/share/applications

ICON       = speed-limit.svg
DESKTOP    = speed-limit.desktop
LAUNCHER   = speed-limit-launcher

.PHONY: all clean install uninstall

all: $(TARGET) $(CLI_TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LIBS)

$(CLI_TARGET): $(CLI_SRC)
	$(CC) -Wall -Wextra -O2 -o $(CLI_TARGET) $(CLI_SRC)

clean:
	rm -f $(TARGET) $(CLI_TARGET)

# Run with: sudo make install
# Installs both binaries to $(BINDIR); only the GUI gets a .desktop entry, so
# the CLI is available globally but does not appear in the application menu.
install: $(TARGET) $(CLI_TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -m 755 $(CLI_TARGET) $(DESTDIR)$(BINDIR)/$(CLI_TARGET)
	install -m 755 $(LAUNCHER) $(DESTDIR)$(BINDIR)/$(LAUNCHER)
	install -d $(DESTDIR)$(ICONDIR)
	install -m 644 $(ICON) $(DESTDIR)$(ICONDIR)/speed-limit.svg
	install -d $(DESTDIR)$(DESKTOPDIR)
	install -m 644 $(DESKTOP) $(DESTDIR)$(DESKTOPDIR)/$(DESKTOP)
	-gtk-update-icon-cache -f -t /usr/share/icons/hicolor 2>/dev/null || true
	-update-desktop-database $(DESKTOPDIR) 2>/dev/null || true
	@echo "Installed. Launch from your menu or run: sudo $(TARGET)"

# Run with: sudo make uninstall
uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(BINDIR)/$(CLI_TARGET)
	rm -f $(DESTDIR)$(BINDIR)/$(LAUNCHER)
	rm -f $(DESTDIR)$(ICONDIR)/speed-limit.svg
	rm -f $(DESTDIR)$(DESKTOPDIR)/$(DESKTOP)
	-gtk-update-icon-cache -f -t /usr/share/icons/hicolor 2>/dev/null || true
	-update-desktop-database $(DESKTOPDIR) 2>/dev/null || true
	@echo "Uninstalled."
