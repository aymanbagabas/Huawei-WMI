_pkgname=huawei-wmi
pkgname="${_pkgname}-git"
pkgver=r37.b8ba36f
pkgrel=1
pkgdesc='Huawei WMI Linux Driver'
arch=('x86_64')
url='https://github.com/aymanbagabas/huawei-wmi'
license=('GPL2')
depends=('linux')
makedepends=('linux-headers')
provides=("${_pkgname}=${pkgver}")

source=("${_pkgname}::git+https://github.com/aymanbagabas/huawei-wmi.git")
sha256sums=('SKIP')

pkgver() {
  cd "${_pkgname}"
  printf "r%s.%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
}

build() {
  cd "${_pkgname}"
  make
}

package() {
  cd "${_pkgname}"

  install -Dt "$pkgdir/usr/lib/modules/extramodules-ARCH" -m644 *.ko
  find "${pkgdir}" -name '*.ko' -exec xz {} +

  install -Dt "$pkgdir/usr/lib/udev/hwdb.d" -m644 99-Huawei.hwdb
}
