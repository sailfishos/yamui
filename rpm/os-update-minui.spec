Name:       os-update-minui
Summary:    Minimal UI tool for displaying simple graphical indicators
Version:    1.0.0
Release:    1
Group:      System/Boot
License:    ASL 2.0
Source0:    %{name}-%{version}.tar.gz

BuildRequires:  libpng-devel
Requires:	libpng

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
