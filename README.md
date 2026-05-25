# install
```
yay -S cjson wayland-protocols fcft pixman

git clone https://github.com/mangowm/mangobar.git
cd mangobar
meson build -Dprefix=/usr/
sudo ninja -C build install
```