Name:       harbour-kelp
Summary:    Secure peer-to-peer file sharing for Sailfish OS
Version: 0.1.0
Release:    1
Group:      Qt/Qt
License:    GPL-3.0-only
URL:        https://github.com/kapitaali/harbour-kelp
Source0:    %{name}-%{version}.tar.bz2

# The "dev extras" feature bundle (src/kelpbuild.h): conveniences meant for
# building and testing that must not ship in a release — today, writing each
# share's kelp:// link, and so that bundle's key, into the log by default. 1 while
# developing; a release build sets this to 0, which is also what a build outside
# this spec gets (harbour-kelp.pro). See README, "Development builds".
%define kelp_dev_extras 1

# The UI uses Sailfish.Pickers (FilePickerPage / FolderPickerPage) from
# sailfish-components-pickers-qt5. Deliberately no Requires for it: the Harbour
# validator rejects the package name and it is a base-image package, so it is
# always present.
#
# Qt5Network is the LAN-direct listener (KelpServer). Qt owns sockets; libcrypto
# stays content-only, which keeps the two OpenSSL copies on Sailfish from having
# anything to do with each other.
#
# libkelp uses OpenSSL's libcrypto for AES-256-GCM and HKDF. Only the -devel
# package is needed to build; the runtime library comes from openssl-libs (a
# base-image package) and rpm works the dependency out on its own. libssl is the
# relay tunnel's TLS 1.3 transport (src/kelptls.cpp) — no Qt-side dependency,
# because Qt 5.6 cannot do TLS 1.3 or client ALPN.
#
# Nemo.KeepAlive (libkeepalive) is the background-serving keepalive, a
# Harbour-allowed module and another base-image package, so again no explicit
# Requires.
Requires:   sailfishsilica-qt5

BuildRequires: pkgconfig(sailfishapp) >= 1.0.2
BuildRequires: pkgconfig(Qt5Core)
BuildRequires: pkgconfig(Qt5Gui)
BuildRequires: pkgconfig(Qt5Network)
BuildRequires: pkgconfig(Qt5Qml)
BuildRequires: pkgconfig(Qt5Quick)
BuildRequires: pkgconfig(libcrypto)
BuildRequires: pkgconfig(libssl)
BuildRequires: desktop-file-utils

%description
Kelp lets you share a folder or file from your phone as a kelp:// link that a
recipient can open in a browser or in the app. Content-addressed and encrypted
by default, with the key travelling in the link fragment. Sharing seals the
selection into on-device objects and serves them on the local network, so a link
works between two devices on one Wi-Fi; the relay and swarm for everything wider
than that are not built yet.

%prep
%setup -q -n %{name}-%{version}

%build
# Hand the git-derived version to the app so the About page shows the real
# thing (APP_VERSION survives qmake untouched via the environment).
export APP_VERSION=%{version}
# The dev-extras bundle travels the same way: qmake reads the environment and
# turns a non-zero value into the KELP_DEV_BUILD define (harbour-kelp.pro).
export KELP_DEV_EXTRAS=%{kelp_dev_extras}
%qmake5 harbour-kelp.pro
%make_build

%install
rm -rf %{buildroot}
%qmake5_install
# The SDK invokes qmake with QMAKE_STRIP=: (no-op), and brp-strip only runs
# `strip -g`, which keeps .symtab — the Harbour validator flags the binary
# as "not stripped". Full-strip it here instead.
%{__strip} %{buildroot}%{_bindir}/harbour-kelp

%files
%defattr(-,root,root,-)
%{_bindir}/harbour-kelp
%{_datadir}/harbour-kelp
%{_datadir}/applications/harbour-kelp.desktop
%{_datadir}/icons/hicolor/*/apps/harbour-kelp.png
