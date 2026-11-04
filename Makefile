
.PHONY: build build-debug run clean default install

default: build

build:
	meson setup build
	ninja -C build

build-debug:
	meson setup build --buildtype=debug
	ninja -C build

install: build
	ninja -C build install

run: build
	./build/mangobar

format:
	git diff --name-only --diff-filter=ACMR | \
		grep -E '\.(c|h|cpp|hpp)$$' | \
		xargs -r clang-format -i

clean:
	rm -rf build