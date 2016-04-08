Name:       yamui
Summary:    Minimal UI tool for displaying simple graphical indicators
Version:    1.0.6
Release:    1
Url:        https://github.com/sailfishos/yamui.git
Group:      System/Boot
License:    ASL 2.0
Source0:    %{name}-%{version}.tar.gz

BuildRequires:  libpng-devel

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
