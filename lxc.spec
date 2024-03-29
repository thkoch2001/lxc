#
# lxc: linux Container library
#
# (C) Copyright IBM Corp. 2007, 2008
#
# Authors:
# Daniel Lezcano <daniel.lezcano at free.fr>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

%global with_python %{?_with_python: 1} %{?!_with_python: 0}
%global with_lua %{?_with_lua: 1} %{?!_with_lua: 0}

# Set with_systemd on distros that use it, so we can install the service
# file, otherwise the sysvinit script will be installed
%if 0%{?fedora} >= 14 || 0%{?rhel} >= 7 || 0%{?suse_version} >= 1210
%global with_systemd 1
%define init_script systemd
BuildRequires: systemd-units
%else
%global with_systemd 0
%define init_script sysvinit
%endif

# RPM needs alpha/beta/rc in Release: not Version: to ensure smooth
# package upgrades from alpha->beta->rc->release. For more info see:
# http://fedoraproject.org/wiki/Packaging%3aNamingGuidelines#NonNumericRelease
%if "x" != "x"
%global beta_rel 
%global beta_dot .%{beta_rel}
%else
%global norm_rel 1
%endif

Name: lxc
Version: 1.0.1
Release: %{?beta_rel:0.1.%{beta_rel}}%{?!beta_rel:%{norm_rel}}%{?dist}
URL: http://linuxcontainers.org
Source: http://linuxcontainers.org/downloads/%{name}-%{version}%{?beta_dot}.tar.gz
Summary: Linux Containers userspace tools
Group: Applications/System
License: LGPLv2+
BuildRoot: %{_tmppath}/%{name}-%{version}-build
Requires: openssl rsync
BuildRequires: libcap libcap-devel docbook2X graphviz

%if %{with_python}
Requires: python3
BuildRequires: python3-devel
%endif

%description
Containers are insulated areas inside a system, which have their own namespace
for filesystem, network, PID, IPC, CPU and memory allocation and which can be
created using the Control Group and Namespace features included in the Linux
kernel.

This package provides the lxc-* tools, which can be used to start a single
daemon in a container, or to boot an entire "containerized" system, and to
manage and debug your containers.

%package	libs
Summary:	Shared library files for %{name}
Group:		System Environment/Libraries
%description	libs
The %{name}-libs package contains libraries for running %{name} applications.

%package	devel
Summary:	Development library for %{name}
Group:		Development/Libraries
Requires:	%{name} = %{version}-%{release}, pkgconfig
%description	devel
The %{name}-devel package contains header files and library needed for
development of the Linux containers.

%if %{with_lua}
%package	lua
Summary:	Lua bindings for %{name}
Group:		System Environment/Libraries
Requires:	lua-filesystem lua-alt-getopt
BuildRequires:	lua-devel
%description	lua
The %{name}-lua package contains %{name} bindings for lua.
%endif

%prep
%setup -q -n %{name}-%{version}%{?beta_dot}
%build
PATH=$PATH:/usr/sbin:/sbin %configure $args \
%if %{with_lua}
  --enable-lua \
%endif
%if %{with_python}
  --enable-python \
%endif
  --disable-rpath \
  --with-init-script=%{init_script}
make %{?_smp_mflags}

%install
rm -rf %{buildroot}
make install DESTDIR=%{buildroot}
find %{buildroot} -type f -name '*.la' -exec rm -f {} ';'

%clean
rm -rf %{buildroot}

%post
%post   libs -p /sbin/ldconfig
%postun libs -p /sbin/ldconfig

%files
%defattr(-,root,root)
%{_bindir}/*
%{_mandir}/man1/lxc*
%{_mandir}/man5/lxc*
%{_mandir}/man7/lxc*
%{_mandir}/ja/man1/lxc*
%{_mandir}/ja/man5/lxc*
%{_mandir}/ja/man7/lxc*
%{_datadir}/doc/*
%{_datadir}/lxc/*
%{_sysconfdir}/bash_completion.d
%config(noreplace) %{_sysconfdir}/lxc/*

%if %{with_systemd}
%{_unitdir}/lxc.service
%else
%{_sysconfdir}/rc.d/init.d/lxc
%endif

%files libs
%defattr(-,root,root)
%{_libdir}/*.so.*
%{_libdir}/%{name}
%if %{with_python}
%{_libdir}/python*
%endif
%{_localstatedir}/*
%{_libexecdir}/%{name}
%attr(4555,root,root) %{_libexecdir}/%{name}/lxc-init
%attr(4111,root,root) %{_libexecdir}/%{name}/lxc-user-nic
%if %{with_systemd}
%attr(555,root,root) %{_libexecdir}/%{name}/lxc-devsetup
%endif

%if %{with_python}
%{_libdir}/python3.3/site-packages/_lxc*
%{_libdir}/python3.3/site-packages/lxc/*
%endif

%if %{with_lua}
%files lua
%defattr(-,root,root)
%{_datadir}/lua
%{_libdir}/lua
%endif

%files devel
%defattr(-,root,root)
%{_includedir}/%{name}/*
%{_libdir}/*.so
%{_libdir}/pkgconfig/*

%changelog
* Tue Oct 22 2013 Dwight Engen <dwight.engen@oracle.com> - 1.0.0-0.1.alpha2
- fix some rpmlint warnings/errors
- split lua bits into seperate package

* Mon Sep 10 2012 Dwight Engen <dwight.engen@oracle.com> - 0.8.0
- fix lxc-init moved to libexec
- .pc moved to _libdir
- package template files /usr/share/lxc/templates

* Thu Sep  8 2011 Greg Kurz <gkurz@fr.ibm.com> - 0.7.5.1
- fix installed files for rpmbuild
- introduce lxc-libs package

* Fri Jul 23 2010 Daniel Lezcano <dlezcano@fr.ibm.com> - 0.7.2
- set attribute for installed files
- fix libraries installation

* Tue Mar 24 2009 Daniel Lezcano <daniel.lezcano@free.fr> - 0.6.1
- Removed capability setting, let the user to do that through "lxc-setcap"

* Mon Feb 16 2009 Daniel Lezcano <daniel.lezcano@free.fr> - 0.6.0
- Added more capabilities to the executables

* Sun Jan 25 2009 Daniel Lezcano <daniel.lezcano@free.fr> - 0.6.0
- Reduced spec file

* Sun Aug 3 2008 Daniel Lezcano <dlezcano@fr.ibm.com> - 0.1.0
- Initial RPM release.

# Local variables:
# mode: shell-script
# sh-shell: rpm
# end:
