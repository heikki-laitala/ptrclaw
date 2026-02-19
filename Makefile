BUILDDIR := builddir

ifeq ($(shell uname),Darwin)
  NATIVE_FILE := meson-native-macos.ini
  CLANG_TIDY_EXTRA := -extra-arg=-stdlib=libc++ -extra-arg=-isysroot$(shell xcrun --show-sdk-path)
  ifeq ($(shell xcode-select -p 2>/dev/null),)
    $(error Xcode Command Line Tools required. Install with: xcode-select --install)
  endif
else
  NATIVE_FILE := meson-native-linux.ini
  CLANG_TIDY_EXTRA :=
endif

.PHONY: deps setup build run test lint clean

deps:
ifeq ($(shell uname),Darwin)
	@command -v brew >/dev/null || { echo "Error: Homebrew is required. Install from https://brew.sh"; exit 1; }
	brew install meson llvm
else
	sudo apt-get update
	sudo apt-get install -y g++ meson ninja-build libcurl4-openssl-dev clang-tidy
endif

setup:
	@if [ ! -d $(BUILDDIR) ]; then meson setup $(BUILDDIR) --native-file $(NATIVE_FILE) -Dcatch2:tests=false; fi

build: setup
	meson compile -C $(BUILDDIR)

run: build
	./$(BUILDDIR)/ptrclaw

test: build
	meson test -C $(BUILDDIR)

lint: build
	run-clang-tidy -quiet -p $(BUILDDIR) $(CLANG_TIDY_EXTRA) -source-filter='^(?!.*subprojects).*\.cpp$$' 2>&1 | grep -v 'warnings generated'

clean:
	rm -rf $(BUILDDIR)
