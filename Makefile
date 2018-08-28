# amidithru - user mode ALSA MIDI thru port.
# Copyright (C) 2018  Vilniaus Blokas UAB, https://blokas.io/
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

all: amidithru

CXXFLAGS ?= -O3
LDFLAGS ?= -lasound

CXX=g++-4.9

amidithru: amidithru.o
	$(CXX) $^ -o $@ $(LDFLAGS)
	strip $@

%.o: %.cpp
	$(CXX) -c $(CXXFLAGS) $^ -o $@

install: all
	@cp -p amidithru $(BINARY_DIR)/

clean:
	rm -f amidithru *.o
	rm -f amidithru.deb
	rm -f debian/usr/bin/amidithru
	gunzip `find . | grep gz` > /dev/null 2>&1 || true

amidithru.deb: amidithru
	@gzip --best -n ./debian/usr/share/doc/amidithru/changelog ./debian/usr/share/doc/amidithru/changelog.Debian ./debian/usr/share/man/man1/amidithru.1
	@mkdir -p debian/usr/bin
	@cp -p amidithru debian/usr/bin/
	@fakeroot dpkg --build debian
	@mv debian.deb amidithru.deb
	@gunzip `find . | grep gz` > /dev/null 2>&1
