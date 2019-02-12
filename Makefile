# amidiauto - ALSA MIDI autoconnect daemon.
# Copyright (C) 2019  Vilniaus Blokas UAB, https://blokas.io/
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; version 2 of the
# License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#

BINARY_DIR ?= /usr/local/bin

all: amidiauto

CXXFLAGS ?= -O3
LDFLAGS ?= -lasound

CXX?=g++-4.9

amidiauto: amidiauto.o
	$(CXX) $^ -o $@ $(LDFLAGS)
	strip $@

%.o: %.cpp
	$(CXX) -c $(CXXFLAGS) $^ -o $@

install: all
	@cp -p amidiauto $(BINARY_DIR)/
	@cp -p amidiauto.service /usr/lib/systemd/system/
	@systemctl daemon-reload > /dev/null 2>&1
	@systemctl enable amidiauto > /dev/null 2>&1
	@systemctl start amidiauto > /dev/null 2>&1

clean:
	rm -f amidiauto *.o
	rm -f amidiauto.deb
	rm -f debian/usr/bin/amidiauto
	gunzip `find . | grep gz` > /dev/null 2>&1 || true

amidiauto.deb: amidiauto
	@gzip --best -n ./debian/usr/share/doc/amidiauto/changelog ./debian/usr/share/doc/amidiauto/changelog.Debian ./debian/usr/share/man/man1/amidiauto.1
	@mkdir -p debian/usr/bin
	@cp -p amidiauto debian/usr/bin/
	@fakeroot dpkg --build debian
	@mv debian.deb amidiauto.deb
	@gunzip `find . | grep gz` > /dev/null 2>&1
