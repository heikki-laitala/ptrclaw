BUILDDIR := builddir
export PATH := /opt/homebrew/opt/llvm/bin:$(PATH)

.PHONY: setup build test lint clean

setup:
	@if [ ! -d $(BUILDDIR) ]; then meson setup $(BUILDDIR) -Dcatch2:tests=false; fi

build: setup
	meson compile -C $(BUILDDIR)

test: build
	meson test -C $(BUILDDIR)

lint: build
	ninja -C $(BUILDDIR) clang-tidy 2>&1 | grep -v 'warnings generated'

clean:
	rm -rf $(BUILDDIR)
