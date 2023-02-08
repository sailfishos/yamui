Name:       yamui
Summary:    Minimal UI tool for displaying simple graphical indicators
Version:    1.2.1
Release:    1
Url:        https://github.com/sailfishos/yamui.git
Group:      System/Boot
License:    ASL 2.0
Source0:    %{name}-%{version}.tar.gz

BuildRequires:  pkgconfig(libpng)
BuildRequires:  pkgconfig(libdrm)
BuildRequires:  pkgconfig(libsystemd)
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(gio-2.0)

%description
%{summary}.

%prep
%setup -q -n %{name}-%{version}

%build
make

%install

%make_install

%files
%defattr(-,root,root,-)
%{_bindir}/%{name}
%{_bindir}/yamui-powerkey
%{_bindir}/yamui-screensaverd
