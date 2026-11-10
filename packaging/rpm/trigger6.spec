# SPDX-License-Identifier: GPL-2.0-only
#
# Fedora packages trigger6-dkms (kernel module source for DKMS) and
# trigger6-fast (helper, udev rule, systemd user service). Build from the
# release tarball, for example in a Fedora container:
#   rpmbuild -ba --define "_sourcedir $PWD" trigger6.spec

Name:           trigger6
Version:        1.2.0
Release:        1%{?dist}
Summary:        Linux DRM driver for MCT Trigger 6 USB to HDMI adapters
License:        GPL-2.0-only
URL:            https://github.com/nvaert1986/trigger6-kernel-module
Source0:        %{url}/archive/refs/tags/v%{version}/trigger6-kernel-module-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  systemd-rpm-macros

%description
Out-of-tree DRM/KMS kernel module and a user-space helper for the MCT
"T6 USB Station" chipset, as in the StarTech USB32HDES USB 3.0 to HDMI
adapter (USB ID 0711:5601).

%package -n trigger6-dkms
Summary:        DRM/KMS driver for MCT Trigger 6 USB to HDMI adapters (DKMS)
BuildArch:      noarch
Requires:       dkms >= 3.0
Requires:       kmod
# headers must be there before %%post builds the module
Requires(post): kernel-devel
# DKMS needs it to create its key and sign the module (Secure Boot)
Recommends:     openssl
Recommends:     trigger6-fast

%description -n trigger6-dkms
Out-of-tree kernel module for the MCT "T6 USB Station" chipset, as in the
StarTech USB32HDES USB 3.0 to HDMI adapter (USB ID 0711:5601): display,
hardware cursor, gamma table and HDMI audio. DKMS builds it for every
installed kernel that has kernel-devel installed.

%package -n trigger6-fast
Summary:        SIMD NV12 helper for the trigger6 USB display driver
# loaded with dlopen(), so rpm cannot find it by itself
Requires:       libyuv.so.0()(64bit)
Recommends:     trigger6-dkms

%description -n trigger6-fast
User-space helper that converts the desktop picture to NV12 with libyuv for
the trigger6 driver (about 60 instead of 29 frames per second at 1920x1080).
A udev rule gives the logged-in user access to the adapter and starts the
helper as a systemd user service when the adapter appears.

%prep
%autosetup -n trigger6-kernel-module-%{version}

%build
%{__cc} %{optflags} -pthread -o trigger6-fast userspace/trigger6-fast.c -ldl %{build_ldflags}

%install
install -d %{buildroot}%{_usrsrc}/trigger6-%{version}/kernel
install -m 0644 kernel/Kbuild kernel/Makefile kernel/*.c kernel/*.h \
	%{buildroot}%{_usrsrc}/trigger6-%{version}/kernel/
install -m 0644 dkms.conf LICENSE README.md %{buildroot}%{_usrsrc}/trigger6-%{version}/
install -Dm 0644 packaging/debian13/trigger6.conf %{buildroot}%{_sysconfdir}/modprobe.d/trigger6.conf
install -Dm 0755 trigger6-fast %{buildroot}%{_bindir}/trigger6-fast
install -Dm 0644 packaging/debian13/71-trigger6.rules %{buildroot}%{_udevrulesdir}/71-trigger6.rules
install -Dm 0644 packaging/debian13/trigger6-fast.service %{buildroot}%{_userunitdir}/trigger6-fast.service

%post -n trigger6-dkms
dkms add -m trigger6 -v %{version} >/dev/null 2>&1 || :
for k in /lib/modules/*; do
	[ -e "$k/build" ] || continue
	dkms install -m trigger6 -v %{version} -k "${k##*/}" || \
		echo "trigger6: DKMS build for ${k##*/} failed" >&2
done
:

%preun -n trigger6-dkms
dkms remove -m trigger6 -v %{version} --all >/dev/null 2>&1 || :

%files -n trigger6-dkms
%license LICENSE
%{_usrsrc}/trigger6-%{version}/
%config(noreplace) %{_sysconfdir}/modprobe.d/trigger6.conf

%files -n trigger6-fast
%license LICENSE
%{_bindir}/trigger6-fast
%{_udevrulesdir}/71-trigger6.rules
%{_userunitdir}/trigger6-fast.service

%changelog
* Tue Nov 10 2026 nvaert1986 <25772781+nvaert1986@users.noreply.github.com> - 1.2.0-1
- Release 1.2.0: partial updates, USB 2 support, optional vblank timer, rectangle writes in flight.

* Fri Nov 06 2026 nvaert1986 <25772781+nvaert1986@users.noreply.github.com> - 1.1.0-1
- Release 1.1.0: HDMI audio, hardware cursor, gamma table, full EDID, ELD and jack.
